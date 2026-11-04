// Code memory shared by a JIT plugin (writer) and its guest (executor).
// Apple uses its per-thread MAP_JIT protection. Android uses the same shared
// backing for distinct RW and RX views, as Horizon's service and game do.
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

typedef struct { void *rx, *rw; size_t size; } dm_jit_memory;

static inline int dm_jit_memory_create(dm_jit_memory *memory, size_t size) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    (void)memory; (void)size;
    errno = ENOTSUP;
    return -1;
#elif defined(__APPLE__)
    void *rx = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (rx == MAP_FAILED) return -1;
    *memory = (dm_jit_memory){rx, rx, size};
    return 0;
#else
    unsigned flags = MFD_CLOEXEC;
#ifdef MFD_EXEC
    flags |= MFD_EXEC;
#endif
    int fd = memfd_create("nxnative-jit", flags);
    if (fd < 0 && flags != MFD_CLOEXEC && errno == EINVAL)
        fd = memfd_create("nxnative-jit", MFD_CLOEXEC);
    if (fd < 0) return -1;
    void *rw = MAP_FAILED, *rx = MAP_FAILED;
    if (!ftruncate(fd, (off_t)size)) {
        rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (rw != MAP_FAILED) rx = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    }
    int error = errno;
    close(fd);
    if (rx == MAP_FAILED) {
        if (rw != MAP_FAILED) munmap(rw, size);
        errno = error;
        return -1;
    }
    *memory = (dm_jit_memory){rx, rw, size};
    return 0;
#endif
}
static inline void dm_jit_memory_write(int enabled) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(!enabled);
#else
    (void)enabled; // The plugin's separate writable view is never executable.
#endif
}
static inline void dm_jit_memory_publish(const dm_jit_memory *memory, size_t offset, size_t size) {
    // Clean stores through the writable alias, then invalidate instructions
    // through the address the guest executes (also used for cheat text writes).
    char *rw = (char *)memory->rw + offset, *rx = (char *)memory->rx + offset;
    __builtin___clear_cache(rw, rw + size);
    if (rw != rx) __builtin___clear_cache(rx, rx + size);
}
static inline void dm_jit_memory_destroy(dm_jit_memory *memory) {
    if (memory->rw != memory->rx) munmap(memory->rw, memory->size);
    munmap(memory->rx, memory->size);
    *memory = (dm_jit_memory){0};
}
