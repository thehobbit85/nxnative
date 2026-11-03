// Exercise the production scheduler, guest stack switch, TLS, and SVC gate on arm64 bionic.
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

int svc_thread(int, uint64_t *);
uint32_t svc_thread_main_init(void);
void guest_thread_init(void);
static _Atomic unsigned phases;
static uint32_t read_end, write_end, worker, main_thread;
static uint8_t *stack;
enum { STACK_SIZE = 65536 };
static void *main_tls, *main_tlr;
static uint64_t worker_id;
static const uintptr_t guest_x18 = 0x12345678;
static const uint64_t marker = 0x4567;
struct dm_flags g_flag;
uint64_t g_t0_ns;
__thread _Atomic int dm_host_work;

// SVC sites have the same call and return convention as the Android rewriter. The ordinary
// gate registry dispatches each by its return PC, on the gate stack allocated by thread_main.
void gate_register(uint64_t, const char *, int, uint64_t);
typedef struct { uint64_t x0, x1; } result_t;
extern result_t gate_call(uint64_t, uint64_t, uint64_t, uint64_t, unsigned);
extern char sites[];
extern void spin(_Atomic unsigned *phase);
extern char spin_begin[], spin_end[];
__asm__(".text\n.p2align 2\n"
        "gate_call: adr x16,sites\nadd x16,x16,x4,lsl #5\nbr x16\n"
        "sites: .rept 0x26\nstp x29,x30,[sp,#-16]!\n"
        "mrs x30,tpidr_el0\nldr x30,[x30,#16]\nldr x30,[x30,#32]\nblr x30\n"
        "ldp x29,x30,[sp],#16\nret\nnop\n.endr\n"
        "spin: mov x9,#0x4567\nfmov d31,x9\nmov w10,#2\nstlr w10,[x0]\n"
        "spin_begin: ldar w10,[x0]\ncmp w10,#3\nb.ne spin_begin\nspin_end: ret\n");
static module_t synthetic;
static _Atomic unsigned gate_visits[0x26];
// Use the production SVC counter hook to check that dispatch is running on the pthread stack.
void hlecount_svc(int num) {
    uintptr_t host_sp = (uintptr_t)&num;
    assert(host_sp < (uintptr_t)stack || host_sp >= (uintptr_t)stack + 3 * STACK_SIZE);
    assert((unsigned)num < 0x26);
    atomic_fetch_add(&gate_visits[num], 1);
}
// Only MapMemory on a foreign allocation can consult the optional Unreal host allocator.
int dm_ue_heap_owns(uint64_t addr, uint64_t bytes) { (void)addr; (void)bytes; abort(); }

static uint64_t gate(uint64_t a, uint64_t b, uint64_t c, uint64_t d, unsigned num) {
    result_t r = gate_call(a, b, c, d, num);
    if (num == 0x25) worker_id = r.x1;
    return r.x0;
}
static void payload(uintptr_t arg) {
    assert(arg == 17 && !atomic_load(&phases));
    assert(dm_slot_get(g_keytls) && main_tls != dm_slot_get(g_keytls));
    const uint8_t *tlr = dm_slot_get(g_keytlr);
    assert(tlr && tlr != main_tlr && *(void **)(tlr + 0x1f8));
    assert(!gate(0, 0xffff8000, 0, 0, 0x25));
    assert(worker_id && worker_id == *(uint32_t *)(tlr + 0x110));
    dm_slot_set(g_key18, (void *)guest_x18);
    assert(gate(0, 0, 0, 0, 0x10) == 0);
    atomic_store(&phases, 1);
    assert(!gate(0, (uintptr_t)&read_end, 1, UINT64_MAX, 0x18));
    assert(gate(0, 0, 0, 0, 0x10) == 1);
    spin(&phases);
    gate(0, 0, 0, 0, 0x0a);
    abort();
}
static void call(unsigned num, uint64_t r[32]) { assert(svc_thread(num, r) && !r[0]); }
// Horizon ThreadContext ABI, kept independent of the production serializer.
typedef struct {
    uint64_t x[29], fp, lr, sp, pc;
    uint32_t pstate, pad;
    __uint128_t v[32];
    uint32_t fpcr, fpsr;
    uint64_t tpidr;
} context_t;
_Static_assert(sizeof(context_t) == 0x320, "ThreadContext");
static _Atomic uint32_t lock_word, cond_key, nreleased;
static uint32_t release_order[3];
static void condition_worker(uintptr_t i) {
    uint32_t thr_cur_handle(void);
    uint32_t tag = thr_cur_handle();
    atomic_store(&lock_word, tag);
    assert(!gate((uintptr_t)&lock_word, (uintptr_t)&cond_key, tag, UINT64_MAX, 0x1c));
    assert(atomic_load(&lock_word) == tag);
    unsigned n = atomic_load(&nreleased);
    assert(n < 3); release_order[n] = i; atomic_store(&nreleased, n + 1);
    assert(!gate((uintptr_t)&lock_word, 0, 0, 0, 0x1b));
    gate(0, 0, 0, 0, 0x0a);
    abort();
}
static void condition_order(void) {
    uint64_t r[32] = {0};
    uint32_t peers[3];
    for (unsigned i = 0; i < 3; i++) {
        r[1] = (uintptr_t)condition_worker; r[2] = i; r[3] = (uintptr_t)stack + (i + 1) * STACK_SIZE;
        r[4] = i == 2 ? 26 : 28; r[5] = 0;
        call(0x08, r); peers[i] = r[1];
        // A not-yet-started thread has its initial argument, stack and entry.
        r[0] = peers[i]; r[1] = 1; call(0x32, r);
        context_t ctx; r[0] = (uintptr_t)&ctx; r[1] = peers[i]; call(0x33, r);
        assert(ctx.pc == (uintptr_t)condition_worker && ctx.sp == (uintptr_t)stack + (i + 1) * STACK_SIZE && ctx.x[0] == i);
        if (i == 0) {   // Starting a suspended thread must leave its initial register image intact.
            r[0] = peers[i]; call(0x09, r);
            r[0] = (uintptr_t)&ctx; r[1] = peers[i]; call(0x33, r);
            assert(ctx.pc == (uintptr_t)condition_worker && ctx.x[0] == i);
        }
        r[0] = peers[i]; r[1] = 0; call(0x32, r);
        if (i) { r[0] = peers[i]; call(0x09, r); }
        else while (!atomic_load(&cond_key) || atomic_load(&lock_word)) sched_yield();
        assert(atomic_load(&lock_word) == 0 && atomic_load(&cond_key) == 1);
    }
    const unsigned expected[] = {2, 0, 1}; // highest priority first, then registration order
    for (unsigned i = 0; i < 3; i++) {
        atomic_store(&lock_word, main_thread);
        r[0] = (uintptr_t)&cond_key; r[1] = 1; call(0x1d, r);
        assert(atomic_load(&lock_word) == (main_thread | 0x40000000u));
        r[0] = (uintptr_t)&lock_word; call(0x1b, r);
        assert(atomic_load(&nreleased) == i + 1 && release_order[i] == expected[i]);
        r[1] = (uintptr_t)&peers[expected[i]]; r[2] = 1; r[3] = UINT64_MAX; call(0x18, r);
        r[0] = peers[expected[i]]; call(0x16, r);
    }
    assert(!atomic_load(&lock_word) && !atomic_load(&cond_key));
}
int main(void) {
    alarm(15);
    for (unsigned i = 0; i < 0x26; i++) {
        uintptr_t site = (uintptr_t)sites + 32 * i;
        gate_register(site + 20, "svc", i, site);
    }
    synthetic.base = (uint8_t *)spin_begin; synthetic.size = (uintptr_t)spin_end - (uintptr_t)spin_begin;
    g_mod = &synthetic; g_nmod = 1;
    guest_thread_init();
    main_tls = dm_slot_get(g_keytls); main_tlr = dm_slot_get(g_keytlr);
    main_thread = svc_thread_main_init();
    uint64_t r[32] = {0};
    call(0x45, r); write_end = r[1]; read_end = r[2];
    stack = mmap(NULL, 3 * STACK_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(stack != MAP_FAILED);
    r[1] = (uintptr_t)payload; r[2] = 17; r[3] = (uintptr_t)stack + STACK_SIZE; r[4] = 30; r[5] = 0;
    call(0x08, r); worker = r[1]; assert(worker != main_thread && !atomic_load(&phases));
    r[0] = worker; r[1] = 28; call(0x0d, r); r[1] = worker; call(0x0c, r); assert(r[1] == 28);
    r[0] = worker; call(0x09, r); // The same-core StartThread handoff must last until the event wait.
    assert(atomic_load(&phases) == 1 && worker_id != *(uint32_t *)((uint8_t *)main_tlr + 0x110));
    context_t ctx;
    r[0] = worker; r[1] = 1; call(0x32, r); // WaitSynchronization is parked in the real gate.
    r[0] = (uintptr_t)&ctx; r[1] = worker; call(0x33, r);
    assert(ctx.pc == (uintptr_t)sites + 32 * 0x18);
    assert(ctx.x[18] == guest_x18 && ctx.x[1] == (uintptr_t)&read_end && ctx.x[2] == 1);
    assert(ctx.sp >= (uintptr_t)stack && ctx.sp < (uintptr_t)stack + STACK_SIZE);
    r[0] = worker; r[1] = 1; r[2] = 2; call(0x0f, r);
    r[0] = worker; r[1] = 0; call(0x32, r);
    r[0] = (uintptr_t)&ctx; r[1] = worker;
    assert(svc_thread(0x33, r) && r[0] == 0xfa01); // The resumed SVC is still waiting; its old pause cannot leak.
    r[0] = write_end; call(0x11, r);
    while (atomic_load(&phases) != 2) sched_yield();
    for (int i = 0; i < 32; i++) {
        r[0] = worker; r[1] = 1; call(0x32, r); // Now it is executing guest instructions.
        r[0] = (uintptr_t)&ctx; r[1] = worker; call(0x33, r);
        assert(ctx.pc >= (uintptr_t)spin_begin && ctx.pc < (uintptr_t)spin_end);
        assert(ctx.x[9] == marker && ctx.x[18] == guest_x18 && (uint64_t)ctx.v[31] == marker);
        assert(ctx.sp >= (uintptr_t)stack && ctx.sp < (uintptr_t)stack + STACK_SIZE);
        r[0] = worker; r[1] = 0; call(0x32, r);
    }
    atomic_store(&phases, 3);
    r[1] = (uintptr_t)&worker; r[2] = 1; r[3] = UINT64_MAX; call(0x18, r);
    r[0] = worker; call(0x16, r); r[0] = write_end; call(0x16, r); r[0] = read_end; call(0x16, r);
    condition_order();
    assert(gate_visits[0x25] == 1 && gate_visits[0x18] == 1 && gate_visits[0x10] == 2);
    assert(gate_visits[0x1b] == 3 && gate_visits[0x1c] == 3 && gate_visits[0x0a] == 4);
    assert(!munmap(stack, 3 * STACK_SIZE));
    puts("PASS: guest stacks/TLS, same-core handoff, event wait, priority, core migration, 32 live pause/resumes, scalar/SIMD context, exit/close, priority/FIFO condition and mutex handoff");
}
