// Execute a synthetic rewritten module on two real pthreads through entry.S and thread.c.
#include "../../hle.h"
#include "../../dm_uc.h"
#include <assert.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern void guest_thread_init(void), guest_thread_set_id(uint64_t), guest_gate_stack(int);
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);
extern const uint8_t dm_gate_stubs[];
extern uint64_t dm_gate_top[];
typedef uint64_t (*guest_callback)(uint64_t);
static guest_callback callback;
static uintptr_t guest_entry;
static uint32_t (*read_steady)(uint8_t *);
static _Atomic unsigned in_gate;
static __thread jmp_buf finished;
static __thread uint64_t tag, result[5], host_x18;
static __thread volatile sig_atomic_t handled;

struct dm_flags g_flag;
// Clock reads use the production gate and host clock. This fixture has no GPU or deterministic frame source.
__thread _Atomic int dm_host_work;
void dm_guest_clock_advanced(void) { abort(); }
int svc_nro_deactivate(module_t *m) { abort(); }
void hlecount_seal(void) {}
extern void loader_hash_log(void);
HLE("test_sdk_call", uint64_t, sdk_call, uint64_t x) { return x + 13; }
void *hlecount_wrap(const char *kind, const char *name, void *fn) { return fn; }
void dm_spin_wait(uint64_t pc, uint64_t orig) { abort(); }
void thr_svc_enter(uint64_t *regs, uint64_t orig) {}
void thr_svc_leave(void) {}
void dm_thread_returned(void) { longjmp(finished, 1); }

uint64_t svc_dispatch(int svc, uint64_t *regs, uint64_t orig) {
    assert(svc == 0x1e && orig < g_mod[0].text_end);
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

extern void crash_thread_init(void), crash_install(void);
extern void crash_set_trap_hook(int (*fn)(ucontext_t *));
static int trap(ucontext_t *uc) {
    uint64_t pc = UC_PC(uc), v[32][2]; uint32_t fpcr, fpsr;
    if (UC_X(uc, 0) != tag || !uc_fpsimd(uc, v, &fpcr, &fpsr) || v[31][0] != tag) _exit(4);
    UC_PC(uc) += 4;
    if (*(uint32_t *)(uintptr_t)pc != 0xd42000e0) _exit(2);  // BRK #7, while the guest has a spill below SP
    stack_t active;
    if (sigaltstack(NULL, &active) || !(active.ss_flags & SS_ONSTACK)) _exit(3);
    handled = 1;
    return 1;
}

static void *worker(void *arg) {
    tag = (uintptr_t)arg;
    result[0] = tag;
    const size_t stack_bytes = 128 * 1024;
    uint8_t *guest_stack = malloc(stack_bytes);
    assert(guest_stack);
    crash_thread_init();
    guest_thread_init();
    guest_thread_set_id(tag);
    __asm__ volatile("mov %0, x18" : "=r"(host_x18));
    assert(callback(37) == tag + 37 + 3 + 13 && (uintptr_t)dm_slot_get(g_key18) == 37);
    struct timespec before, after;
    uint8_t timepoint[24];
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    assert(read_steady(timepoint) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    int64_t seconds;
    memcpy(&seconds, timepoint, sizeof seconds);
    assert(seconds >= before.tv_sec && seconds <= after.tv_sec);
    assert(!memcmp(timepoint + 8, "dmhost-steady-01", 16));
    uint64_t tick_before = now_ns() * 12 / 625;
    if (!setjmp(finished)) {
        guest_gate_stack(1);
        dm_thread_enter(guest_entry, (uintptr_t)result, (uintptr_t)(guest_stack + stack_bytes));
        abort();
    }
    assert(handled);
    assert(result[4] >= tick_before && result[4] <= now_ns() * 12 / 625);
    assert(result[0] == tag + 1 && result[1] == tag && result[2] == tag && result[3] == tag + 2);
    free(guest_stack);
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    alarm(15);
    const char *names[] = {"main", "sdk", "clockprobe"};
    load_modules(argv[1], names, 3);
    loader_hash_log();
    guest_entry = (uintptr_t)guest_sym("guest_entry");
    callback = (guest_callback)guest_sym("guest_callback");
    read_steady = guest_sym("read_steady");
    assert(guest_entry && callback && read_steady && !dm_sdk_ounce());
    crash_install();
    crash_set_trap_hook(trap);
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, worker, (void *)101) && !pthread_create(&b, NULL, worker, (void *)202));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    puts("Guest timer counter, registered SDK steady clock, linked module callbacks, TLS, trap and SVC return on two threads: OK");
    return 0;
}
