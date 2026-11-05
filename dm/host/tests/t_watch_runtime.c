// The actual watch backend, memory SVC dispatcher and renderer dirty tracker.
#define TEST_REAL_DIRTY
#define main memory_fixture_main
#define memory_fault fixture_memory_fault
#include "t_svc_memory.c"
#undef main
#undef memory_fault
#include "../watch_store.h"
static _Atomic int hold_admission, admission_waiting, admit;
static _Atomic int hold_unprotect, unprotect_waiting, unprotect_continue;
static int runtime_acquire(dm_watch_page *p) {
    if (atomic_exchange(&hold_admission, 0)) {
        atomic_store(&admission_waiting, 1); while (!atomic_load(&admit)) sched_yield();
    }
    return dm_watch_page_acquire(p);
}
static int runtime_protect(void *p, size_t n, int protection) {
    if (protection == (PROT_READ | PROT_WRITE) && atomic_exchange(&hold_unprotect, 0)) {
        atomic_store(&unprotect_waiting, 1); while (!atomic_load(&unprotect_continue)) sched_yield();
    }
    return mprotect(p, n, protection);
}
#define dm_watch_page_acquire runtime_acquire
#define mprotect runtime_protect
#include "../watch_android.c"
#undef mprotect
#undef dm_watch_page_acquire

static _Atomic unsigned translation_faults, permission_faults;
static void runtime_fault(int sig, siginfo_t *si, void *ctx) {
    unsigned fsc = uc_esr(ctx) & 63;
    if (si->si_code == SEGV_ACCERR && fsc >= 4 && fsc < 8) atomic_fetch_add(&translation_faults, 1);
    if (si->si_code == SEGV_ACCERR && fsc >= 12 && fsc < 16) atomic_fetch_add(&permission_faults, 1);
    for (unsigned i = 0; i < nfault_hook; i++) if (fault_hook[i](si->si_addr, 1, ctx)) return;
    // Only a failed test reaches this diagnostic, never the production handler.
    ucontext_t *uc = ctx; fprintf(stderr, "unhandled sig=%d code=%d esr=%llx pc=%llx instruction=%08x gate=%llx state=%d\n", sig, si->si_code,
        (unsigned long long)uc_esr(uc), (unsigned long long)UC_PC(uc), *(uint32_t *)UC_PC(uc),
        (unsigned long long)atomic_load(&g_watch_page.gate), atomic_load(&g_watch_disarmed));
    _exit(128 + sig);
}

static void put64(uintptr_t p, uint64_t value) {
    __asm__ volatile("str %1, [%0]" :: "r"(p), "r"(value) : "memory");
}
static void readonly(uintptr_t p, int expected) {
    FILE *f = fopen("/proc/self/maps", "r"); assert(f);
    char line[512], perm[5]; unsigned long lo, hi; int found = 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) == 3 && p >= lo && p < hi) {
        assert(perm[0] == 'r' && (perm[1] == '-') == expected); found = 1;
    }
    fclose(f); assert(found);
}
static _Atomic int retiring, retired;
static void *retire_worker(void *arg) {
    (void)arg; atomic_store(&retiring, 1); assert(!resize(0)); atomic_store(&retired, 1); return NULL;
}
static _Thread_local uint8_t worker_stack[65536] __attribute__((aligned(16)));
static void *delayed_writer(void *arg) {
    stack_t ss = {.ss_sp = worker_stack, .ss_size = sizeof worker_stack}; assert(!sigaltstack(&ss, NULL));
    uintptr_t at = heap + 0x80; uint64_t value = 0xabcdef;
    if (arg) __asm__ volatile("stlr %1, [%0]" :: "r"(at), "r"(value) : "memory");
    else put64(at, value);
    return NULL;
}
static void reject_faults(uintptr_t at) {
    ucontext_t uc = {0}; uint32_t head[] = {0x45535201, 16};
    memcpy(uc.uc_mcontext.__reserved, head, sizeof head);
    const uint64_t esrs[] = {UINT64_C(0x92000061), UINT64_C(0x92000050), UINT64_C(0x92000051), UINT64_C(0x8200004f)};
    for (unsigned i = 0; i < sizeof esrs / sizeof *esrs; i++) {
        memcpy(uc.uc_mcontext.__reserved + 8, esrs + i, 8);
        assert(!watch_fault((void *)at, 1, &uc));
    }
    uint64_t esr = 0x9200004f; memcpy(uc.uc_mcontext.__reserved + 8, &esr, 8);
    assert(!watch_fault((void *)at, 0, &uc));
    assert(!watch_fault((void *)(at + 2 * getpagesize()), 1, &uc));
    assert(!(atomic_load(&g_watch_page.gate) & DM_WATCH_PAGE_COUNT));
}
int main(void) {
    alarm(20);
    assert(!setenv("DM_PROBE_WATCH", "heap+80+8", 1));
    struct sigaction sa = {.sa_sigaction = runtime_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    uint8_t *alt = malloc(65536); assert(alt);
    stack_t ss = {.ss_sp = alt, .ss_size = 65536}; assert(!sigaltstack(&ss, NULL));
    sigemptyset(&sa.sa_mask); assert(!sigaction(SIGSEGV, &sa, NULL));
    dm_android_watch_install();
    mem_init(); heap = info(I_HeapAddr); alias = info(I_AliasAddr); stack = info(I_StackAddr);
    uintptr_t at = heap + 0x80; size_t pg = (size_t)getpagesize();
    for (unsigned i = 0; i < 64; i++) {
        assert(!resize(0x200000)); readonly(at, 1);
        if (i & 1) assert(*(uint64_t *)at == 0); // other iterations write before any guest PTE exists
        else assert(*(uint64_t *)(g_watch_page.alias + 0x80) == 0);
        put64(at, 0xabc000 + i); assert(*(uint64_t *)at == 0xabc000 + i); readonly(at, 1);
        uint8_t *old = mmap(NULL, pg, PROT_READ, MAP_SHARED, g_watch_page.fd, 0); assert(old != MAP_FAILED);
        assert(!resize(0)); uint8_t byte; assert(!dm_host_read(at, &byte, 1));
        assert(*(uint64_t *)(old + 0x80) == 0xabc000 + i); assert(!munmap(old, pg));
    }
    assert(!resize(0x200000));
    reject_faults(at);
    assert(dm_watch_page_acquire(&g_watch_page));
    pthread_t thread; assert(!pthread_create(&thread, NULL, retire_worker, NULL));
    while (!atomic_load(&retiring) || (atomic_load(&g_watch_page.gate) & DM_WATCH_PAGE_OPEN)) sched_yield();
    assert(!atomic_load(&retired));
    *(uint64_t *)(g_watch_page.alias + 0x80) = 0x7788;
    dm_watch_page_release(&g_watch_page); assert(!pthread_join(thread, NULL));
    assert(atomic_load(&retired));
    assert(!resize(0x200000)); assert(*(uint64_t *)at == 0);
    uint64_t value = 0x8877; assert(!dm_guest_heap_write(at, &value, sizeof value));
    assert(*(uint64_t *)at == value); readonly(at, 0);
    assert(!resize(0)); assert(!resize(0x200000)); readonly(at, 1);
    __asm__ volatile("stlr %1, [%0]" :: "r"(at), "r"(value) : "memory");
    assert(*(uint64_t *)at == value); readonly(at, 0);
    assert(!resize(0)); assert(!resize(0x200000)); readonly(at, 1);
    put64(heap + pg - 4, value); uint64_t cross; memcpy(&cross, (void *)(heap + pg - 4), 8);
    assert(cross == value); readonly(at, 0);
    assert(!resize(0)); assert(!resize(0x200000)); readonly(at, 1);
    // Pause the first unsupported-store handler inside its RW transition. A
    // second handler must complete it without waiting for the paused owner.
    atomic_store(&hold_unprotect, 1);
    assert(!pthread_create(&thread, NULL, delayed_writer, (void *)1));
    while (!atomic_load(&unprotect_waiting)) sched_yield();
    __asm__ volatile("stlr %1, [%0]" :: "r"(at), "r"(value) : "memory");
    readonly(at, 0); atomic_store(&unprotect_continue, 1); assert(!pthread_join(thread, NULL));
    assert(!resize(0)); assert(!resize(0x200000)); readonly(at, 1);
    // Holding stderr must not block a faulting writer. Overflow is counted;
    // handlers never wait for this logger or overwrite an undrained event.
    flockfile(stderr);
    for (unsigned i = 0; i < 256; i++) put64(at, i);
    assert(atomic_load(&g_watch_dropped)); funlockfile(stderr);
    readonly(at, 1);
    // The real dirty tracker takes protection ownership before it protects.
    atomic_store(&hold_admission, 1);
    assert(!pthread_create(&thread, NULL, delayed_writer, NULL));
    while (!atomic_load(&admission_waiting)) sched_yield();
    nvn_dirty_add_range((void *)heap, pg);
    atomic_store(&admit, 1); assert(!pthread_join(thread, NULL));
    assert(*(uint64_t *)at == 0xabcdef);
    readonly(at, 0);
    nvn_dirty_protect((void *)heap, pg); readonly(at, 1);
    // A second overlapping range must not undo the first range's protection.
    nvn_dirty_add_range((void *)(heap + 16), pg - 16); readonly(at, 1);
    put64(at, 0x1234); readonly(at, 0);
    nvn_dirty_remove_range((void *)(heap + 16));
    nvn_dirty_remove_range((void *)heap);
    assert(!resize(0)); assert(!resize(0x200000)); readonly(at, 0);
    put64(at, 0x5678); assert(*(uint64_t *)at == 0x5678);
    assert(!resize(0));
    assert(atomic_load(&translation_faults) && atomic_load(&permission_faults));
    alarm(0);
    puts("PASS: real memory SVC watch integration, 64 lifetimes, lease drain, disarm/recommit, bounded logging and dirty-tracker ownership");
    return 0;
}
