// Optional diagnostic hooks for standalone host fixtures. The Android runtime
// links watch_android.c explicitly; Apple retains its Mach watch implementation.
// Include after dm.h. Open/release calls are serialized by the memory owner.
#pragma once
#ifdef __ANDROID__
enum { DM_WATCH_HEAP = 1, DM_WATCH_MODULE = 2, DM_WATCH_ALIAS = 3, DM_WATCH_STACK = 4 };
void dm_android_watch_region(unsigned kind, void *base, size_t size) __attribute__((weak));
void dm_android_watch_module(module_t *module) __attribute__((weak));
int dm_android_watch_open(void *address, size_t size) __attribute__((weak));
void dm_android_watch_release(uintptr_t lo, uintptr_t hi) __attribute__((weak));
// Only for synchronous owner copies immediately following preparation. Other
// preparation hooks still run; the copy uses the guest view and faults normally.
void dm_android_watch_cpu_prepare(void *address, size_t size) __attribute__((weak));
// Bound known owner copies to the watched page. A large libc copy may otherwise
// issue one paired store across its edge and require disabling the diagnostic.
void dm_android_watch_copy(void *dst, const void *src, size_t size) __attribute__((weak));
void dm_android_watch_zero(void *dst, size_t size) __attribute__((weak));
// Acknowledge inherited protection in the new dirty descriptor before closing
// watch admission, so a delayed old watch fault is recognized by the new owner.
void dm_android_watch_dirty_range(const void *address, size_t size,
                                 void (*acknowledge)(void *, uintptr_t), void *owner) __attribute__((weak));
void dm_android_watch_install(void) __attribute__((weak));
#endif
