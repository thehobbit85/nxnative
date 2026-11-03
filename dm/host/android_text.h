// Android's loader reserves the full NSO anonymously. Back just main's text
// with shareable pages. Keep the executable guest view RX and open a separate
// RW, non-executable view only when a cheat file needs it (hle_cheats.c).
#pragma once
#include "dm.h"
#include <sys/mman.h>
#include <unistd.h>

static inline void dm_copy_image(module_t *m, const uint8_t *src, size_t bytes, size_t rx) {
    if (!m->nro && !strcmp(m->name, "main") && rx && rx <= m->size) {
        unsigned flags = MFD_CLOEXEC;
#ifdef MFD_EXEC
        flags |= MFD_EXEC;
#endif
        int fd = memfd_create("nxnative-main-text", flags);
        if (flags != MFD_CLOEXEC && fd < 0 && errno == EINVAL)
            fd = memfd_create("nxnative-main-text", MFD_CLOEXEC);   // before Linux 6.3
        if (fd >= 0 && !ftruncate(fd, (off_t)rx)) {
            uint8_t *rw = mmap(NULL, rx, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (rw != MAP_FAILED) {
                size_t code = bytes < rx ? bytes : rx;
                memcpy(rw, src, code);
                __builtin___clear_cache((char *)rw, (char *)rw + code);
                if (mmap(m->base, rx, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED) {
                    munmap(rw, rx);
                    m->text_fd_plus_one = fd + 1;   // zero-initialized module; fd 0 is valid
                    if (bytes > rx) memcpy(m->base + rx, src + rx, bytes - rx);
                    return;
                }
                // MAP_FIXED can discard the old mapping on failure. Restore
                // the original, still empty reservation before the plain copy.
                munmap(rw, rx);
                if (mmap(m->base, rx, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
                    die("cannot restore main text allocation");
            }
        }
        if (fd >= 0) close(fd);
        LOG("[RED] cheats: shared executable text unavailable, using private text (data cheats remain available)");
    }
    memcpy(m->base, src, bytes);
}
