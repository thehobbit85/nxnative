// Exercise real Linux signals and force a handler to outlive its caller's wait.
#include "../dm.h"
#include "../host_memory.h"
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
static int snapshot_read(uintptr_t address, void *out, size_t bytes);
static int snapshot_test_clock(clockid_t clock, struct timespec *now);
#define dm_host_read snapshot_read
#define clock_gettime snapshot_test_clock
#include "../thread_snapshot.h"
#undef dm_host_read
#undef clock_gettime

static _Atomic int worker_tid, stop, hold, entered, release_handler, blocked, unblock;
static _Atomic unsigned turns;
static _Atomic int freeze_deadline;
static int delayed_result;
module_t *module_of(const void *p) { (void)p; return NULL; }
static int snapshot_test_clock(clockid_t clock, struct timespec *now) {
    if (atomic_load(&freeze_deadline)) { now->tv_sec = 1; now->tv_nsec = 0; return 0; }
    return clock_gettime(clock, now);
}
static int snapshot_read(uintptr_t address, void *out, size_t bytes) {
    if (atomic_load(&hold) && (int)syscall(SYS_gettid) == atomic_load(&worker_tid)) {
        atomic_store(&entered, 1);
        while (!atomic_load(&release_handler)) sched_yield();
    }
    return dm_host_read(address, out, bytes);
}
static void on_signal(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info; int saved = errno; snapshot_signal(context); errno = saved;
}
static void *worker(void *unused) {
    (void)unused; atomic_store(&worker_tid, (int)syscall(SYS_gettid));
    while (!atomic_load(&stop)) { atomic_fetch_add(&turns, 1); __asm__ volatile("" ::: "memory"); }
    return NULL;
}
static void *block_signal(void *unused) {
    (void)unused; sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR2);
    assert(!pthread_sigmask(SIG_BLOCK, &mask, NULL)); atomic_store(&blocked, 1);
    while (!atomic_load(&unblock)) sched_yield();
    assert(!pthread_sigmask(SIG_UNBLOCK, &mask, NULL));
    return NULL;
}
static void *competing(void *unused) {
    (void)unused;
    for (int i = 0; i < 40; i++) assert(snapshot_dump("competing", 0) >= -1);
    return NULL;
}
static void *delayed_dump(void *unused) { (void)unused; delayed_result = snapshot_dump("delayed", 0); return NULL; }
static void invalid_frames(void) {
    size_t page = (size_t)getpagesize();
    uint8_t *area = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(area != MAP_FAILED && !mprotect(area + page, page, PROT_NONE));
    uint64_t *cycle = (uint64_t *)area; cycle[0] = (uintptr_t)cycle; cycle[1] = 0x3333;
    uintptr_t frames[] = {0, (uintptr_t)area + page, (uintptr_t)area + page - 8, (uintptr_t)cycle};
    uint64_t owner = (uint64_t)(unsigned)syscall(SYS_gettid) << 32;
    atomic_store(&snap_used, 1); atomic_store(&snap_active, 1);
    for (unsigned i = 0; i < sizeof frames / sizeof *frames; i++) {
        ucontext_t uc = {0}; UC_PC(&uc) = 0x1111; UC_LR(&uc) = 0x2222; UC_SP(&uc) = 0x4444; UC_FP(&uc) = frames[i];
        atomic_store(&snap_slots[0].request, owner | SNAP_REQUEST); snapshot_signal(&uc);
        assert(atomic_load(&snap_slots[0].request) == (owner | SNAP_DONE));
        assert(snap_slots[0].frame[0] == 0x1111 && snap_slots[0].frame[1] == 0x2222 && snap_slots[0].sp == 0x4444);
        assert(snap_slots[0].frame[2] == (i == 3 ? 0x3333u : 0));
        for (int k = 3; k < SNAP_FRAMES; k++) assert(!snap_slots[0].frame[k]);
    }
    atomic_store(&snap_active, 0); assert(!munmap(area, 2 * page));
}
int main(void) {
    alarm(20);
    fputs("Snapshot fixture started\n", stderr);
    struct sigaction sa = {.sa_sigaction = on_signal, .sa_flags = SA_SIGINFO | SA_RESTART};
    sigemptyset(&sa.sa_mask); assert(!sigaction(SIGUSR2, &sa, NULL)); atomic_store(&snap_ready, 1);
    invalid_frames();
    pthread_t thread; assert(!pthread_create(&thread, NULL, worker, NULL));
    while (!atomic_load(&turns)) sched_yield();
    for (int i = 0; i < 16; i++) assert(snapshot_dump("repeated", 0) >= 1);
    atomic_store(&hold, 1); atomic_store(&freeze_deadline, 1);
    pthread_t inspecting; assert(!pthread_create(&inspecting, NULL, delayed_dump, NULL));
    while (!atomic_load(&entered)) sched_yield();
    assert(snapshot_dump("overlap", 0) == -1); // First caller is still inspecting, not merely a stress loop.
    atomic_store(&freeze_deadline, 0); assert(!pthread_join(inspecting, NULL) && delayed_result >= 0);
    assert(snapshot_dump("pending", 0) == -1); // No reuse of an unfinished writer.
    unsigned before = atomic_load(&turns); atomic_store(&hold, 0); atomic_store(&release_handler, 1);
    while (atomic_load(&turns) == before) sched_yield();
    assert(snapshot_dump("recovered", 0) >= 1);
    pthread_t block; assert(!pthread_create(&block, NULL, block_signal, NULL));
    while (!atomic_load(&blocked)) sched_yield();
    uint64_t start = snapshot_clock(); assert(snapshot_dump("blocked", 0) >= 1);
    assert(snapshot_clock() - start < 1000000000); // One blocked thread cannot hang diagnostics.
    atomic_store(&unblock, 1); assert(!pthread_join(block, NULL));
    assert(snapshot_dump("late", 0) >= 1);
    pthread_t peer; assert(!pthread_create(&peer, NULL, competing, NULL));
    for (int i = 0; i < 40; i++) assert(snapshot_dump("concurrent", 0) >= -1);
    assert(!pthread_join(peer, NULL));
    assert(snapshot_dump("final", 0) >= 1);
    atomic_store(&stop, 1); assert(!pthread_join(thread, NULL));
    puts("Thread snapshots: real signals, repeated/concurrent requests, bounded blocked delivery, delayed writer and safe reuse: OK");
}
