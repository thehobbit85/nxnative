// Native filesystem operations used by save transactions and portable save bundles.
#pragma once
#include <errno.h>
#include <stdio.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/clonefile.h>
#define FS_GUEST_NAME "org.d2rmobile.guest-name"
#define FS_NO_ATTR ENOATTR
#define fs_get_name(p, v, n) getxattr((p), FS_GUEST_NAME, (v), (n), 0, XATTR_NOFOLLOW)
#define fs_set_name(p, v, n) setxattr((p), FS_GUEST_NAME, (v), (n), 0, XATTR_NOFOLLOW)
#define fs_remove_name(p) removexattr((p), FS_GUEST_NAME, XATTR_NOFOLLOW)
static inline int fs_clone_tree(const char *src, const char *dst) { return clonefile(src, dst, 0); }
static inline int fs_swap_tree(const char *a, const char *b) { return renamex_np(a, b, RENAME_SWAP); }
#else
#define FS_GUEST_NAME "user.org.d2rmobile.guest-name"
#define FS_NO_ATTR ENODATA
#define fs_get_name(p, v, n) lgetxattr((p), FS_GUEST_NAME, (v), (n))
#define fs_set_name(p, v, n) lsetxattr((p), FS_GUEST_NAME, (v), (n), 0)
#define fs_remove_name(p) lremovexattr((p), FS_GUEST_NAME)
int fs_clone_tree(const char *src, const char *dst);
int fs_swap_tree(const char *a, const char *b);
#endif
