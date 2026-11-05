// Optional diagnostic hooks for standalone host fixtures. The Android runtime
// links watch_android.c explicitly; Apple retains its Mach watch implementation.
// Include after dm.h. Open/release calls are serialized by the memory owner.
#pragma once
#ifdef __ANDROID__
void dm_android_watch_heap_reserved(void *heap, size_t size) __attribute__((weak));
void dm_android_watch_module(module_t *module) __attribute__((weak));
int dm_android_watch_open(void *address, size_t size) __attribute__((weak));
void dm_android_watch_release(uintptr_t lo, uintptr_t hi) __attribute__((weak));
// Acknowledge inherited protection in the new dirty descriptor before closing
// watch admission, so a delayed old watch fault is recognized by the new owner.
void dm_android_watch_dirty_range(const void *address, size_t size,
                                 void (*acknowledge)(void *, uintptr_t), void *owner) __attribute__((weak));
void dm_android_watch_install(void) __attribute__((weak));
#endif
