// The fd monitor resolves real open files without changing their offsets.
#include "../hle_fdstat.c"
#include <assert.h>
int main(void) {
    const char *tmp = getenv("TMPDIR");
    char name[MAXPATHLEN], path[MAXPATHLEN], resolved[MAXPATHLEN];
    snprintf(name, sizeof name, "%s/dm-fdstat.XXXXXX", tmp ? tmp : "/tmp");
    int fd = mkstemp(name); assert(fd >= 0);
    assert(write(fd, "ok", 2) == 2 && realpath(name, resolved));
    assert(fd_path(fd, path) && !strcmp(resolved, path));
    assert(lseek(fd, 0, SEEK_CUR) == 2);
    close(fd);
    assert(!fd_path(fd, path));
    unlink(name);
    puts("Open descriptor path and offset preserved, closed descriptor rejected");
}
