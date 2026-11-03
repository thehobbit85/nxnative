// Execute a synthetic rewritten module on two real pthreads through entry.S and thread.c.
#include "../../hle.h"
#include "guest_image.h"
#include <assert.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#ifndef __ANDROID__
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
#endif
extern void guest_thread_init(void), guest_thread_set_id(uint64_t), guest_gate_stack(int);
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);
extern const uint8_t dm_gate_stubs[];
extern uint64_t dm_gate_top[];
static uint8_t *image;
static _Atomic unsigned in_gate;
static __thread jmp_buf finished;
static __thread uint64_t tag, result[4], host_x18;
static __thread volatile sig_atomic_t handled;

struct dm_flags g_flag;
extern void gate_register(uint64_t pc, const char *kind, int arg, uint64_t orig);
extern uint64_t resolve_import(module_t *m, const char *name);
static uint64_t host_import;
HLE("test_sdk_call", uint64_t, sdk_call, uint64_t x) { return x + 13; }
void *hlecount_wrap(const char *kind, const char *name, void *fn) { return fn; }
uint64_t guest_ticks(void) { abort(); }
void guest_clock_read_wait(void) { abort(); }
void dm_spin_wait(uint64_t pc, uint64_t orig) { abort(); }
void thr_svc_enter(uint64_t *regs, uint64_t orig) {}
void thr_svc_leave(void) {}
void dm_thread_returned(void) { longjmp(finished, 1); }

uint64_t svc_dispatch(int svc, uint64_t *regs, uint64_t orig) {
    assert(svc == 0x1e && orig == (uintptr_t)image + GUEST_SVC_PC);
    uintptr_t entry = (uintptr_t)dm_slot_get(g_keygate);
    size_t slot = (entry - (uintptr_t)dm_gate_stubs) / 16;
    assert(slot < 1024 && dm_gate_top[slot] == (uintptr_t)regs + 800);
    assert(regs[0] == tag && regs[2] == tag && regs[4] == tag && regs[18] == host_x18);
    assert((uintptr_t)dm_slot_get(g_key18) == tag && (uintptr_t)dm_slot_get(g_keytls) == tag);
    assert(regs[3] == (uintptr_t)dm_slot_get(g_keytlr));
    assert(*(uint32_t *)(uintptr_t)(regs[3] + 0x110) == tag);

    // Both register frames and both thread slots must stay live until both dispatches are inside the gate.
    atomic_fetch_add(&in_gate, 1);
    while (atomic_load(&in_gate) != 2) sched_yield();
    assert(regs[0] == tag && regs[2] == tag);
    regs[0] = tag + 1;
    regs[17] = tag + 2;
    return regs[0];
}

static void trap(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    ucontext_t *uc = context;
#ifdef __APPLE__
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    uc->uc_mcontext->__ss.__pc += 4;
#else
    uint64_t pc = uc->uc_mcontext.pc;
    uc->uc_mcontext.pc += 4;
#endif
    if (*(uint32_t *)(uintptr_t)pc != 0xd42000e0) _exit(2);  // BRK #7, while the guest has a spill below SP
    stack_t active;
    if (sigaltstack(NULL, &active) || !(active.ss_flags & SS_ONSTACK)) _exit(3);
    handled = 1;
}

static void *worker(void *arg) {
    tag = (uintptr_t)arg;
    result[0] = tag;
    const size_t stack_bytes = 128 * 1024;
    uint8_t *guest_stack = malloc(stack_bytes);
    stack_t alt = {.ss_sp = malloc(stack_bytes), .ss_size = stack_bytes};
    assert(guest_stack && alt.ss_sp && !sigaltstack(&alt, NULL));
    guest_thread_init();
    guest_thread_set_id(tag);
    __asm__ volatile("mov %0, x18" : "=r"(host_x18));
    typedef uint64_t (*guest_callback)(uint64_t, uint64_t, uint64_t);
    guest_callback callback = (guest_callback)(image + GUEST_CALLBACK_PC);
    assert(callback(37, 0, host_import) == tag + 37 + 13 && (uintptr_t)dm_slot_get(g_key18) == 37);
    if (!setjmp(finished)) {
        guest_gate_stack(1);
        dm_thread_enter((uintptr_t)image, (uintptr_t)result, (uintptr_t)(guest_stack + stack_bytes));
        abort();
    }
    assert(handled);
    assert(result[0] == tag + 1 && result[1] == tag && result[2] == tag && result[3] == tag + 2);
    alt.ss_flags = SS_DISABLE;
    assert(!sigaltstack(&alt, NULL));
    free(alt.ss_sp);
    free(guest_stack);
    return NULL;
}

int main(void) {
    alarm(15);  // a failed two-thread rendezvous must fail the test
#ifndef __ANDROID__
    assert(!pthread_key_create(&g_key18, NULL) && !pthread_key_create(&g_keytls, NULL));
    assert(!pthread_key_create(&g_keytlr, NULL) && !pthread_key_create(&g_keygate, NULL));
#endif
    image = mmap(NULL, sizeof guest_image, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(image != MAP_FAILED);
    memcpy(image, guest_image, sizeof guest_image);
    uint64_t *page = (uint64_t *)(image + GUEST_KEY_PAGE);
    page[GUEST_K18] = g_key18 * 8; page[GUEST_KTLS] = g_keytls * 8;
    page[GUEST_KTLR] = g_keytlr * 8; page[GUEST_KGATE] = g_keygate * 8;
    __builtin___clear_cache((char *)image, (char *)image + sizeof guest_image);
    assert(!mprotect(image, sizeof guest_image, PROT_READ | PROT_EXEC));
    host_import = resolve_import(NULL, "test_sdk_call");
    assert(host_import == (uintptr_t)sdk_call);
    gate_register((uintptr_t)image + GUEST_GATE_PC, "svc", 0x1e, (uintptr_t)image + GUEST_SVC_PC);
    struct sigaction sa = {.sa_sigaction = trap, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGTRAP, &sa, NULL));
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, worker, (void *)101) && !pthread_create(&b, NULL, worker, (void *)202));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(!munmap(image, sizeof guest_image));
    puts("Rewritten guest callback, separate TLS and gate stacks, signal spill and SVC return on two threads: OK");
    return 0;
}
