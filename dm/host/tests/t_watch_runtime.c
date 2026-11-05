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
#ifdef TEST_PAGE_16K
#define getpagesize memory_getpagesize
#endif
#include "../watch_android.c"
#undef getpagesize
#undef mprotect
#undef dm_watch_page_acquire
#ifdef TEST_PAGE_16K
// Linked by the dirty tracker compiled with the same logical page-size seam.
// This still runs on a 4 KiB kernel; actual 16 KiB qualification is separate.
int dm_test_native_pagesize(void) { return memory_getpagesize(); }
#endif

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
    assert(!watch_fault((void *)(at + 2 * memory_getpagesize()), 1, &uc));
    assert(!(atomic_load(&g_watch_page.gate) & DM_WATCH_PAGE_COUNT));
}
static void watch_alias(void) {
    size_t pg = memory_getpagesize(); uintptr_t at = alias + 0x80;
    uint8_t byte; assert(!dm_host_read(at, &byte, 1)); // prepared does not mean exposed
    for (unsigned i = 0; i < 64; i++) {
        assert(!pages(1, alias, 2 * pg)); readonly(at, 1);
        assert(*(uint64_t *)at == 0); put64(at, 0xaaa000 + i);
        put64(alias + pg + 0x80, 0xbbb000 + i);
        uint8_t *old = mmap(NULL, pg, PROT_READ, MAP_SHARED, g_watch_page.fd, 0); assert(old != MAP_FAILED);
        assert(!pages(0, alias, pg)); assert(!dm_host_read(at, &byte, 1));
        assert(*(uint64_t *)(old + 0x80) == 0xaaa000 + i);
        assert(*(uint64_t *)(alias + pg + 0x80) == 0xbbb000 + i);
        assert(!pages(1, alias, pg)); readonly(at, 1); assert(*(uint64_t *)at == 0);
        assert(!munmap(old, pg)); assert(!pages(0, alias, 2 * pg));
    }
    if (pg > P) {
        assert(!pages(1, alias, P)); readonly(at, 1); uint64_t generation = g_watch_page.generation;
        put64(at, 0x1234); put64(alias + P + 0x80, 0xbad);
        assert(!pages(1, alias + P, P)); readonly(at, 1);
        assert(*(uint64_t *)at == 0x1234 && *(uint64_t *)(alias + P + 0x80) == 0);
        put64(alias + P + 0x80, 0x5678);
        assert(!pages(0, alias, P)); assert(g_watch_page.generation == generation);
        assert(!pages(1, alias, P)); readonly(at, 1);
        assert(g_watch_page.generation == generation && *(uint64_t *)at == 0 && *(uint64_t *)(alias + P + 0x80) == 0x5678);
        assert(!pages(0, alias, 2 * P)); assert(!dm_host_read(at, &byte, 1));
    }
    puts("PASS: alias watch, 64 release/recommit lifetimes, old backing and live sibling preservation");
}
static void watch_stack(void) {
    uintptr_t at = stack + 0x80; uint8_t byte;
    assert(!dm_host_read(at, &byte, 1)); assert(!resize(0x200000));
    // A multi-page bionic memcpy uses paired SIMD stores which can straddle a
    // watched 4 KiB page even when both input ranges are page-aligned.
    memset((void *)heap, 0x5a, 4 * P);
    assert(!map(stack, heap, 4 * P)); readonly(at, 1);
    assert(!memcmp((void *)stack, (void *)heap, 4 * P));
    put64(at, 0x7654); assert(!unmap(stack, heap, 4 * P));
    assert(*(uint64_t *)(heap + 0x80) == 0x7654); readonly(at, 1);
    for (unsigned i = 0; i < 64; i++) {
        put64(heap + 0x80, 0x123000 + i);
        assert(!map(stack, heap, P)); readonly(at, 1);
        assert(*(uint64_t *)at == 0x123000 + i);
        put64(at, 0x456000 + i); assert(!unmap(stack, heap, P)); readonly(at, 1);
        assert(*(uint64_t *)(heap + 0x80) == 0x456000 + i && g_watch_page.generation == 1);
    }
    if (memory_getpagesize() > P) {
        put64(heap + P + 0x80, 0xcafe);
        assert(!map(stack, heap, P) && !map(stack + P, heap + P, P)); readonly(at, 1);
        put64(at, 0x1234); put64(stack + P + 0x80, 0x5678);
        assert(!unmap(stack, heap, P)); readonly(at, 1);
        assert(*(uint64_t *)(stack + P + 0x80) == 0x5678);
        assert(!unmap(stack + P, heap + P, P)); readonly(at, 1);
        assert(*(uint64_t *)(heap + P + 0x80) == 0x5678);
    }
    assert(!resize(0));
    puts("PASS: stack watch, 64 MapMemory/copyback cycles without remapping live backing");
}
static void watch_startup(const char *mode) {
    uintptr_t at = heap + 0x80; uint8_t byte;
    assert(!dm_host_read(at, &byte, 1));
    if (!strcmp(mode, "late")) {
        assert(!resize(0x200000)); readonly(at, 0); put64(at, 0x1234);
        dm_android_watch_install(); readonly(at, 1); assert(*(uint64_t *)at == 0x1234);
        put64(at, 0x5678); readonly(at, 1); assert(!resize(0));
    } else {
        dm_android_watch_install(); assert(!dm_host_read(at, &byte, 1));
        nvn_dirty_add_range((void *)heap, memory_getpagesize());
        assert(!dm_host_read(at, &byte, 1)); // dirty exclusion cannot expose a reservation
        assert(!resize(0x200000)); readonly(at, 0);
        nvn_dirty_protect((void *)heap, getpagesize()); readonly(at, 1);
        put64(at, 0x9abc); readonly(at, 0);
        nvn_dirty_remove_range((void *)heap); assert(!resize(0));
    }
    puts("PASS: watch installation/exclusion preserves prepared, exposed and protected states");
}
static unsigned observed_preparations;
static int nest_preparation;
static void observe_preparation(void *p, size_t n) {
    observed_preparations++;
    if (nest_preparation) { nest_preparation = 0; dm_android_watch_cpu_prepare(p, n); }
}
int main(int argc, char **argv) {
    alarm(20);
    const char *mode = argc > 1 ? argv[1] : "heap";
    int startup = !strcmp(mode, "late") || !strcmp(mode, "reserved");
    char setting[64]; snprintf(setting, sizeof setting, "%s+80+8", startup ? "heap" : mode);
    assert(!setenv("DM_PROBE_WATCH", setting, 1));
    struct sigaction sa = {.sa_sigaction = runtime_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    uint8_t *alt = malloc(65536); assert(alt);
    stack_t ss = {.ss_sp = alt, .ss_size = 65536}; assert(!sigaltstack(&ss, NULL));
    sigemptyset(&sa.sa_mask); assert(!sigaction(SIGSEGV, &sa, NULL));
    if (!startup) dm_android_watch_install();
    mem_init(); heap = info(I_HeapAddr); alias = info(I_AliasAddr); stack = info(I_StackAddr);
    if (startup) { watch_startup(mode); alarm(0); return 0; }
    if (!strcmp(mode, "alias")) { watch_alias(); alarm(0); return 0; }
    if (!strcmp(mode, "stack")) { watch_stack(); alarm(0); return 0; }
    uintptr_t at = heap + 0x80; size_t pg = (size_t)memory_getpagesize();
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
    dm_guest_write_prep_register(observe_preparation); nest_preparation = 1;
    uint64_t value = 0x8877; assert(!dm_guest_heap_write(at, &value, sizeof value));
    assert(observed_preparations == 2 && !g_watch_cpu_copy);
    assert(*(uint64_t *)at == value); readonly(at, 1); // known CPU copies still intercept stores
    int fd[2]; assert(!pipe(fd)); value = 0x9988; assert(write(fd[1], &value, 8) == 8);
    dm_guest_write_prepare((void *)at, 8); assert(read(fd[0], (void *)at, 8) == 8);
    close(fd[0]); close(fd[1]); assert(*(uint64_t *)at == value); readonly(at, 0);
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
    if (pg > P) { nvn_dirty_add_range((void *)(heap + P + 16), P); readonly(at, 1); }
    put64(at, 0x1234); readonly(at, 0);
    if (pg > P) nvn_dirty_remove_range((void *)(heap + P + 16));
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
