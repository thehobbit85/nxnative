// The memory SVC reports the calling pthread's live stack and reads saved frame
// pointers which can point outside the guest reservation. Invalid reads must fail.
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
static inline int dm_host_read(uintptr_t address, void *buf, size_t size) {
    vm_size_t got;
    return vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)buf, &got) == KERN_SUCCESS && got == size;
}
static inline size_t dm_host_stack(uintptr_t *base) {
    pthread_t self = pthread_self();
    size_t size = pthread_get_stacksize_np(self);
    *base = (uintptr_t)pthread_get_stackaddr_np(self) - size;
    return size;
}
#else
#include <sys/uio.h>
static inline int dm_host_read(uintptr_t address, void *buf, size_t size) {
    struct iovec local = {buf, size}, remote = {(void *)address, size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
}
static inline size_t dm_host_stack(uintptr_t *base) {
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr)) return 0;
    void *start;
    size_t size;
    int err = pthread_attr_getstack(&attr, &start, &size);
    pthread_attr_destroy(&attr);
    if (err) return 0;
    *base = (uintptr_t)start;
    return size;
}
#endif
