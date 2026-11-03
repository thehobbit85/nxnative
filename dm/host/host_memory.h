// The memory SVC reports the calling pthread's live stack and reads saved frame
// pointers which can point outside the guest reservation. Invalid reads must fail.
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
static inline size_t dm_host_read_some(uintptr_t address, void *buf, size_t size);
static inline int dm_host_read(uintptr_t address, void *buf, size_t size) { return dm_host_read_some(address, buf, size) == size; }
#ifdef __APPLE__
#include <mach/mach.h>
static inline size_t dm_host_read_some(uintptr_t address, void *buf, size_t size) {
    vm_size_t got;
    return vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)buf, &got) == KERN_SUCCESS ? got : 0;
}
static inline int dm_host_write(uintptr_t address, const void *buf, size_t size) {
    return size <= UINT32_MAX && vm_write(mach_task_self(), address, (vm_offset_t)buf, (mach_msg_type_number_t)size) == KERN_SUCCESS;
}
static inline size_t dm_host_stack(uintptr_t *base) {
    pthread_t self = pthread_self();
    size_t size = pthread_get_stacksize_np(self);
    *base = (uintptr_t)pthread_get_stackaddr_np(self) - size;
    return size;
}
#else
#include <sys/uio.h>
#include <errno.h>
static inline size_t dm_host_read_some(uintptr_t address, void *buf, size_t size) {
    struct iovec local = {buf, size}, remote = {(void *)address, size};
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return n > 0 ? (size_t)n : 0;
}
static inline int dm_host_write(uintptr_t address, const void *buf, size_t size) {
    struct iovec local = {(void *)buf, size}, remote = {(void *)address, size};
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
}
static inline size_t dm_host_stack(uintptr_t *base) {
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr)) return 0;
    void *start;
    size_t size;
    int err = pthread_attr_getstack(&attr, &start, &size);
#ifdef __ANDROID__
    // Bionic includes its inaccessible guard in a worker's reported extent.
    // It also retains the requested guard size for a caller-supplied stack
    // even though it did not insert a guard. The main stack can grow down.
    size_t guard;
    if (!err && gettid() != getpid() && !pthread_attr_getguardsize(&attr, &guard) && guard && guard < size) {
        unsigned char byte;
        if (!dm_host_read((uintptr_t)start, &byte, 1) && errno == EFAULT) {
            start = (char *)start + guard;
            size -= guard;
        }
    }
#endif
    pthread_attr_destroy(&attr);
    if (err) return 0;
    *base = (uintptr_t)start;
    return size;
}
#endif
