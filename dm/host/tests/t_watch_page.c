// Android's real memfd/mmap permissions and watch-handler lease lifetimes.
// Failures are injected only at allocation boundaries; existing guest pages
// and separately retained aliases must survive every failed preparation.
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
static int fail_fd, fail_size, fail_map, map_calls, fail_protect, damage_map;
static _Atomic int delay_cas, cas_waiting, cas_continue;
static int observed_cas(_Atomic uint64_t *p, uint64_t *expected, uint64_t next, memory_order success, memory_order failure) {
    if (atomic_load(&delay_cas)) { atomic_store(&cas_waiting, 1); while (!atomic_load(&cas_continue)) sched_yield(); }
    return atomic_compare_exchange_weak_explicit(p, expected, next, success, failure);
}
static int test_memfd(const char *name, unsigned flags) { if (fail_fd) { errno = EMFILE; return -1; } return memfd_create(name, flags); }
static int test_truncate(int fd, off_t n) { if (fail_size) { errno = ENOSPC; return -1; } return ftruncate(fd, n); }
static void *test_map(void *at, size_t n, int prot, int flags, int fd, off_t off) {
    map_calls++;
    if (map_calls == damage_map) { assert(flags & MAP_FIXED); assert(!munmap(at, n)); errno = ENOMEM; return MAP_FAILED; }
    if (map_calls == fail_map) { errno = ENOMEM; return MAP_FAILED; } return mmap(at, n, prot, flags, fd, off);
}
static int test_protect(void *at, size_t n, int prot) { if (fail_protect) { errno = EACCES; return -1; } return mprotect(at, n, prot); }
#undef atomic_compare_exchange_weak_explicit
#define atomic_compare_exchange_weak_explicit observed_cas
#define memfd_create test_memfd
#define ftruncate test_truncate
#define mmap test_map
#define mprotect test_protect
#include "../watch_page.h"
#undef mprotect
#undef mmap
#undef ftruncate
#undef memfd_create
static int fd_count(void) {
    DIR *d = opendir("/proc/self/fd"); assert(d); int n = 0; struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    assert(!closedir(d)); return n;
}
static int can_read(void *p) {
    uint8_t b; struct iovec local = {&b,1}, remote = {p,1}; return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == 1;
}
static int can_write(void *p) {
    uint8_t b = 0x6d; struct iovec local = {&b,1}, remote = {p,1};
    (void)process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == 1;
}
static _Atomic int retire_stage;
static int acquired_after_replacement;
static void *delayed_acquire(void *arg) {
    dm_watch_page *p = arg; acquired_after_replacement = dm_watch_page_acquire(p);
    if (acquired_after_replacement) dm_watch_page_release(p); return NULL;
}
static void *retire_thread(void *arg) {
    atomic_store(&retire_stage, 1); assert(!dm_watch_page_retire(arg)); atomic_store(&retire_stage, 2); return NULL;
}
static void lifetimes(size_t size) {
    // Reserve extra bytes and choose an aligned logical page, including when
    // exercising 16 KiB ownership on a 4 KiB kernel (not real16KiB qualification).
    uint8_t *allocation = mmap(NULL, size * 5, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(allocation != MAP_FAILED);
    uint8_t *guest = (uint8_t *)(((uintptr_t)allocation + size * 2 - 1) & ~(size - 1));
    assert(!mprotect(guest - size, size, PROT_READ | PROT_WRITE)); memset(guest - size, 0x71, size);
    assert(!mprotect(guest + size, size, PROT_READ | PROT_WRITE)); memset(guest + size, 0x72, size);
    dm_watch_page watch = {.fd = -1}; int baseline = fd_count(); uint64_t previous = 0;
    for (unsigned cycle = 0; cycle < 64; cycle++) {
        assert(!dm_watch_page_prepare(&watch, guest, size) && !can_read(guest));
        assert(watch.generation > previous); previous = watch.generation;
        assert(fd_count() == baseline + 1 && !dm_watch_page_acquire(&watch));
        assert(!dm_watch_page_open(&watch) && can_read(guest) && !can_write(guest));
        assert(dm_watch_page_acquire(&watch));
        for (size_t i = 0; i < size; i++) assert(!watch.alias[i] && !guest[i]);
        watch.alias[0] = 0x31; watch.alias[size - 1] = 0x32;
        assert(guest[0] == 0x31 && guest[size - 1] == 0x32);
        uint8_t *old = mmap(NULL, size, PROT_READ, MAP_SHARED, watch.fd, 0); assert(old != MAP_FAILED);
        atomic_store(&retire_stage, 0); pthread_t thread; assert(!pthread_create(&thread, NULL, retire_thread, &watch));
        while (atomic_load(&watch.gate) & DM_WATCH_PAGE_OPEN) sched_yield();
        assert(atomic_load(&retire_stage) == 1 && !dm_watch_page_acquire(&watch));
        // The retiring owner cannot unmap the old alias while a signal handler
        // holds it, and a last in-flight store stays with that allocation.
        watch.alias[1] = 0x33; assert(old[1] == 0x33); dm_watch_page_release(&watch);
        assert(!pthread_join(thread, NULL) && atomic_load(&retire_stage) == 2);
        assert(!watch.alias && watch.fd == -1 && !can_read(guest) && !can_write(guest));
        assert(fd_count() == baseline && old[0] == 0x31 && old[1] == 0x33 && old[size - 1] == 0x32);
        assert(!dm_watch_page_prepare(&watch, guest, size) && !dm_watch_page_open(&watch));
        assert(!guest[0] && !guest[1] && !guest[size - 1] && old[0] == 0x31 && old[1] == 0x33);
        assert(!dm_watch_page_retire(&watch) && !munmap(old, size));
        assert(!dm_watch_page_retire(&watch)); // Empty retirement is harmless.
        assert(guest[-1] == 0x71 && guest[size] == 0x72 && can_write(guest - size) && can_write(guest + size));
    }
    assert(fd_count() == baseline && !munmap(allocation, size * 5));
}
static void failures(void) {
    size_t size = (size_t)getpagesize(); uint8_t *guest = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(guest != MAP_FAILED);
    dm_watch_page watch = {.fd = -1}; int baseline = fd_count();
    assert(dm_watch_page_prepare(&watch, guest + 1, size) == -1 && errno == EINVAL);
    assert(dm_watch_page_prepare(&watch, guest, 0) == -1 && errno == EINVAL);
    for (int mode = 0; mode < 5; mode++) {
        fail_fd = mode == 0; fail_size = mode == 1; fail_map = mode == 2 ? 1 : mode == 3 ? 2 : 0; map_calls = 0;
        damage_map = mode == 4 ? 2 : 0;
        assert(dm_watch_page_prepare(&watch, guest, size) == -1);
        assert(!watch.alias && watch.fd == -1 && !watch.generation && !can_read(guest) && !can_write(guest) && fd_count() == baseline);
        assert(!mprotect(guest, size, PROT_NONE)); // Inaccessible is not enough: the reservation must still exist.
        fail_fd = fail_size = fail_map = damage_map = 0;
    }
    damage_map = 2; fail_map = 3; map_calls = 0;
    assert(dm_watch_page_prepare(&watch, guest, size) == -2 && errno == ENOMEM && fd_count() == baseline);
    assert(mprotect(guest, size, PROT_NONE) == -1); // Owner must abort after this distinct failure.
    damage_map = fail_map = 0;
    assert(mmap(guest, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == guest);
    assert(!dm_watch_page_prepare(&watch, guest, size));
    assert(dm_watch_page_prepare(&watch, guest, size) == -1 && errno == EBUSY);
    fail_protect = 1; assert(dm_watch_page_open(&watch) == -1 && errno == EACCES && !can_read(guest) && !dm_watch_page_acquire(&watch));
    fail_protect = 0; assert(!dm_watch_page_open(&watch));
    fail_protect = 1; assert(dm_watch_page_retire(&watch) == -1 && errno == EACCES && watch.alias && watch.fd >= 0);
    fail_protect = 0;
    assert(!dm_watch_page_retire(&watch) && fd_count() == baseline); assert(!munmap(guest, size));
}
static void stale_admission(void) {
    size_t size = getpagesize(); void *guest = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(guest != MAP_FAILED);
    dm_watch_page watch = {.fd = -1}; assert(!dm_watch_page_prepare(&watch, guest, size) && !dm_watch_page_open(&watch));
    atomic_store(&delay_cas, 1); pthread_t thread; assert(!pthread_create(&thread, NULL, delayed_acquire, &watch));
    while (!atomic_load(&cas_waiting)) sched_yield(); // Handler saw OPEN but has not acquired its lease.
    assert(!dm_watch_page_retire(&watch));
    assert(!dm_watch_page_prepare(&watch, guest, size) && !dm_watch_page_open(&watch));
    atomic_store(&delay_cas, 0); atomic_store(&cas_continue, 1); assert(!pthread_join(thread, NULL));
    assert(!acquired_after_replacement); // An old attempt cannot move a stale fault into the new allocation.
    assert(dm_watch_page_acquire(&watch)); dm_watch_page_release(&watch);
    assert(!dm_watch_page_retire(&watch) && !munmap(guest, size));
}
int main(void) {
    alarm(30); failures(); stale_admission(); lifetimes((size_t)getpagesize()); if (getpagesize() < 16384) lifetimes(16384);
    puts("PASS: watch alias permissions, drained handler leases, fresh generations, preserved old aliases, neighboring pages and allocation-failure cleanup");
    return 0;
}
