// Linux/Android backing for one managed memory-write watch. The owning
// allocator serializes prepare/open/retire, and prepares only fresh inaccessible
// pages before exposure. Signal handlers lease the alias without host locks.
// This is not a facility for converting an already-live anonymous allocation.
// Initialize with {.fd = -1} (all other fields zero) and retain the descriptor
// across every admission attempt. Handlers read its ordinary fields only while
// holding a successful lease. A failed retirement is incomplete, never reusable.
// Guest allocation lifetime still needs ordinary synchronization: a fault that
// has not yet loaded the gate cannot be attributed to a previous generation.
#pragma once
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#define DM_WATCH_PAGE_OPEN (UINT64_C(1) << 63)
#define DM_WATCH_PAGE_COUNT UINT64_C(0xffffffff)
#define DM_WATCH_PAGE_GENERATION UINT64_C(0x7fffffff00000000)
typedef struct {
    uint8_t *guest, *alias;
    size_t size;
    int fd;
    uint64_t generation;
    _Atomic uint64_t gate; // admission bit, generation and active handler count
} dm_watch_page;
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && sizeof(unsigned long) == 8, "watch leases need lock-free 64-bit atomics");

static inline int dm_watch_page_acquire(dm_watch_page *p) {
    uint64_t value = atomic_load_explicit(&p->gate, memory_order_acquire);
    uint64_t generation = value & DM_WATCH_PAGE_GENERATION;
    while ((value & DM_WATCH_PAGE_OPEN) && (value & DM_WATCH_PAGE_COUNT) != DM_WATCH_PAGE_COUNT &&
           (value & DM_WATCH_PAGE_GENERATION) == generation)
        if (atomic_compare_exchange_weak_explicit(&p->gate, &value, value + 1, memory_order_acquire, memory_order_relaxed)) return 1;
    return 0;
}
static inline void dm_watch_page_release(dm_watch_page *p) {
    atomic_fetch_sub_explicit(&p->gate, 1, memory_order_release);
}
// Owner only, never a signal handler. Close admission in the same atomic word
// as the count: an already-observed OPEN cannot race a new lease past the drain.
static inline void dm_watch_page_quiesce(dm_watch_page *p) {
    atomic_fetch_and_explicit(&p->gate, ~DM_WATCH_PAGE_OPEN, memory_order_acq_rel);
    while (atomic_load_explicit(&p->gate, memory_order_acquire) & DM_WATCH_PAGE_COUNT) sched_yield();
}
// -1 leaves the owner's original fresh reservation intact. -2 means even its
// restoration failed: the owner must abort rather than expose an address hole.
static inline int dm_watch_page_prepare(dm_watch_page *p, void *guest, size_t size) {
    if (!guest || !size || (size & (size - 1)) || size < (size_t)getpagesize() || ((uintptr_t)guest & (size - 1))) { errno = EINVAL; return -1; }
    if (p->alias || (atomic_load_explicit(&p->gate, memory_order_relaxed) & (DM_WATCH_PAGE_OPEN | DM_WATCH_PAGE_COUNT))) { errno = EBUSY; return -1; }
    // Never wrap an admission cookie: a delayed CAS must not acquire a different
    // allocation after the old generation was retired and replaced (ABA).
    if (p->generation == 0x7fffffff) { errno = EOVERFLOW; return -1; }
    int fd = memfd_create("nxnative-watch", MFD_CLOEXEC);
    if (fd < 0) return -1;
    uint8_t *alias = MAP_FAILED; int result = -1;
    if (!ftruncate(fd, (off_t)size)) {
        alias = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (alias != MAP_FAILED) {
            if (mmap(guest, size, PROT_NONE, MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED) {
                p->guest = guest; p->alias = alias; p->size = size; p->fd = fd; p->generation++;
                atomic_store_explicit(&p->gate, p->generation << 32, memory_order_release);
                return 0;
            }
            int error = errno;
            // MAP_FIXED failure may have removed the original mapping. This
            // page was empty and inaccessible, so restore precisely that state.
            if (mmap(guest, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) result = -2;
            else errno = error;
        }
    }
    int error = errno;
    if (alias != MAP_FAILED) munmap(alias, size);
    close(fd); errno = error; return result;
}
static inline int dm_watch_page_open(dm_watch_page *p) {
    if (!p->alias) { errno = EINVAL; return -1; }
    if (atomic_load_explicit(&p->gate, memory_order_relaxed) & (DM_WATCH_PAGE_OPEN | DM_WATCH_PAGE_COUNT)) { errno = EBUSY; return -1; }
    // Publish the alias before the protected guest view can generate a fault.
    atomic_store_explicit(&p->gate, DM_WATCH_PAGE_OPEN | (p->generation << 32), memory_order_release);
    if (!mprotect(p->guest, p->size, PROT_READ)) return 0;
    int error = errno; dm_watch_page_quiesce(p); errno = error; return -1;
}
static inline int dm_watch_page_retire(dm_watch_page *p) {
    dm_watch_page_quiesce(p);
    if (!p->alias) return 0;
    // An in-flight handler finishes before permissions/backing change. The
    // owner subsequently replaces its entire released range as usual. Any
    // external view of this old backing remains intact; never zero it here.
    if (mprotect(p->guest, p->size, PROT_NONE)) return -1;
    if (munmap(p->alias, p->size)) return -1;
    close(p->fd); p->alias = NULL; p->fd = -1;
    return 0;
}
