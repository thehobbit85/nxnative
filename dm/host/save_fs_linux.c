// Android saves: copy an independent tree before publishing it in one rename.
#ifdef __linux__
#define _GNU_SOURCE 1
#include "save_fs.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>

int fs_swap_tree(const char *a, const char *b) {
    return (int)syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, RENAME_EXCHANGE);
}

// Preserve user metadata (including guest names). The destination's own policy
// assigns its SELinux label. Linux user xattrs apply to regular files and directories.
static int copy_attrs(int src, int dst) {
    ssize_t n = flistxattr(src, NULL, 0);
    if (n < 0 && errno == ENOTSUP) return 0;
    if (n <= 0) return (int)n;
    char *names = malloc((size_t)n);
    if (!names) return -1;
    n = flistxattr(src, names, (size_t)n);
    int rc = n < 0 ? -1 : 0;
    for (ssize_t pos = 0; !rc && pos < n; pos += (ssize_t)strlen(names + pos) + 1) {
        const char *name = names + pos;
        if (strncmp(name, "user.", 5)) continue;
        ssize_t len = fgetxattr(src, name, NULL, 0);
        if (len < 0) { rc = -1; break; }
        void *data = malloc(len ? (size_t)len : 1);
        if (!data) { rc = -1; break; }
        ssize_t got = fgetxattr(src, name, data, (size_t)len);
        if (got != len) { if (got >= 0) errno = EAGAIN; rc = -1; }
        else rc = fsetxattr(dst, name, data, (size_t)len, 0);
        int e = errno; free(data); errno = e;
    }
    int e = errno; free(names); errno = e; return rc;
}

// Keep the transfer buffer off recursive directory frames on the fixed-size HLE stack.
static __attribute__((noinline)) int copy_bytes(int in, int out) {
    unsigned char data[16384];
    for (;;) {
        ssize_t n = read(in, data, sizeof data);
        if (!n) return 0;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        for (ssize_t pos = 0; pos < n;) {
            ssize_t put = write(out, data + pos, (size_t)(n - pos));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) { if (!put) errno = EIO; return -1; }
            pos += put;
        }
    }
}

static int copy_link(int src, const char *name, int dst, const char *to, const struct timespec times[2]) {
    char *link = malloc(PATH_MAX + 1);
    if (!link) return -1;
    ssize_t n = readlinkat(src, name, link, PATH_MAX + 1);
    int rc = -1;
    if (n > PATH_MAX) errno = ENAMETOOLONG;
    else if (n >= 0) {
        link[n] = 0;
        if (!symlinkat(link, dst, to)) {
            rc = utimensat(dst, to, times, AT_SYMLINK_NOFOLLOW);
            if (rc) { int e = errno; unlinkat(dst, to, 0); errno = e; }
        }
    }
    int e = errno; free(link); errno = e; return rc;
}

static int copy_node(int src, const char *name, int dst, const char *to);
static int remove_node(int parent, const char *name);

// Consume the directory stream; all descendant opens and unlinks stay relative
// to directory descriptors. Cleanup also unlinks symlinks without following them.
static int walk(DIR *dir, int dst) {
    int parent = dirfd(dir), rc = 0;
    while (!rc) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) rc = -1; break; }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        rc = dst < 0 ? remove_node(parent, name) : copy_node(parent, name, dst, name);
    }
    int e = errno; int closed = closedir(dir);
    if (rc) { errno = e; return -1; }
    return closed;
}

static int remove_node(int parent, const char *name) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return (errno == ENOTDIR || errno == ELOOP) ? unlinkat(parent, name, 0) : -1;
    DIR *d = fdopendir(fd);
    if (!d) { int e = errno; close(fd); errno = e; return -1; }
    // Already-copied siblings may have read-only modes.
    if (fchmod(fd, 0700)) { int e = errno; closedir(d); errno = e; return -1; }
    return walk(d, -1) ? -1 : unlinkat(parent, name, AT_REMOVEDIR);
}

static int copy_node(int src, const char *name, int dst, const char *to) {
    struct stat st;
    if (fstatat(src, name, &st, AT_SYMLINK_NOFOLLOW)) return -1;
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    if (S_ISLNK(st.st_mode)) return copy_link(src, name, dst, to, times);
    int dir = S_ISDIR(st.st_mode);
    if (!dir && !S_ISREG(st.st_mode)) { errno = ENOTSUP; return -1; }
    int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (dir ? O_DIRECTORY : O_NONBLOCK);
    int in = openat(src, name, flags);
    if (in < 0) return -1;
    int out = -1, created = 0, rc = -1, e;
    // A replacement between lookup and open cannot supply data or metadata.
    struct stat opened;
    if (fstat(in, &opened)) goto done;
    if (st.st_dev != opened.st_dev || st.st_ino != opened.st_ino) { errno = EAGAIN; goto done; }
    if (dir) {
        if (mkdirat(dst, to, 0700)) goto done;
        created = 1;
        out = openat(dst, to, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } else {
        out = openat(dst, to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        created = out >= 0;
    }
    if (out < 0) goto done;
    if (dir) {
        int fd = dup(in);
        if (fd < 0) goto done;
        DIR *d = fdopendir(fd);
        if (!d) { int e = errno; close(fd); errno = e; goto done; }
        if (walk(d, out)) goto done;
    } else if (copy_bytes(in, out)) goto done;
    if (copy_attrs(in, out) || fchmod(out, st.st_mode & 07777) || futimens(out, times)) goto done;
    rc = 0;
done:
    e = errno;
    if (out >= 0 && close(out) && !rc) { rc = -1; e = errno; }
    close(in);
    if (rc && created) remove_node(dst, to);
    errno = e; return rc;
}

int fs_clone_tree(const char *src, const char *dst) {
    return copy_node(AT_FDCWD, src, AT_FDCWD, dst);
}
#endif
