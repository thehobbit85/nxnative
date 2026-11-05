// Exercise production crash reporting in child processes, not fabricated contexts.
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return g_nmod && a >= (uintptr_t)g_mod->base && a - (uintptr_t)g_mod->base < g_mod->size ? g_mod : NULL;
}
void crash_install(void);
void crash_set_dump_hook(void (*fn)(void));
void crash_set_wait_dump(void (*fn)(void));
static void services(void) { fputs("[fixture] service state\n", stderr); }
static void waits(void) { fputs("[fixture] wait state\n", stderr); }
static _Atomic int locked;
static void *hold_stderr(void *arg) {
    (void)arg; flockfile(stderr); atomic_store(&locked, 1);
    for (;;) pause();
    return NULL;
}

enum { DUMP = 1000, LOCKED, STALL, DEADLINE, GUEST_FAULT };
static void child(int kind) {
    unsetenv("DM_WATCH"); unsetenv("DM_WATCH_STALL");
    if (kind == STALL) assert(!setenv("DM_WATCH_STALL", "1", 1));
    if (kind == DEADLINE) assert(!setenv("DM_WATCH", "1", 1));
    crash_install(); crash_set_dump_hook(services); crash_set_wait_dump(waits);
    crash_watch_progress(73, "diagnostic fixture");
    if (kind == DUMP) { crash_dump_now("fixture request"); _exit(0); }
    if (kind == STALL || kind == DEADLINE) { for (;;) pause(); }
    if (kind == LOCKED) {
        pthread_t t; assert(!pthread_create(&t, NULL, hold_stderr, NULL));
        while (!atomic_load(&locked)) sched_yield();
        raise(SIGABRT); _exit(1);
    }
    if (kind == GUEST_FAULT) {
        size_t pg = (size_t)getpagesize();
        uint8_t *image = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(image != MAP_FAILED);
        const uint32_t code[] = {0xf9000001, 0xd65f03c0}; // str x1, [x0]; ret
        memcpy(image, code, sizeof code);
        assert(!mprotect(image, pg, PROT_READ | PROT_EXEC) && !mprotect(image + pg, pg, PROT_NONE));
        __builtin___clear_cache((char *)image, (char *)image + sizeof code);
        module_t m = {.name="diagnostic", .base=image, .size=2 * pg};
        g_mod = &m; g_nmod = 1;
        ((void (*)(void *, uint64_t))image)(image + pg, UINT64_C(0x13579));
    } else raise(kind);
    _exit(1);
}

static void check(int kind) {
    int fd[2]; assert(!pipe(fd));
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { close(fd[0]); assert(dup2(fd[1], STDERR_FILENO) == STDERR_FILENO); close(fd[1]); child(kind); }
    close(fd[1]); char log[65536]; size_t n = 0;
    for (;;) {
        assert(n < sizeof log - 1);
        ssize_t got = read(fd[0], log + n, sizeof log - 1 - n);
        if (got < 0 && errno == EINTR) continue;
        assert(got >= 0); if (!got) break; n += (size_t)got;
    }
    log[n] = 0; close(fd[0]);
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    int sig = kind == LOCKED ? SIGABRT : (kind == STALL || kind == DEADLINE) ? SIGALRM : kind;
#ifdef __APPLE__
    if (kind == GUEST_FAULT) sig = SIGBUS; // Darwin reports a protected mapping as EXC_BAD_ACCESS / SIGBUS.
#else
    if (kind == GUEST_FAULT) sig = SIGSEGV;
#endif
    if (WEXITSTATUS(status) != (kind == DUMP ? 0 : 128 + sig)) fprintf(stderr, "case %d exited %d:\n%s", kind, WEXITSTATUS(status), log);
    assert(WEXITSTATUS(status) == (kind == DUMP ? 0 : 128 + sig));
    if (kind == DUMP) {
        assert(strstr(log, "state dump (fixture request): tick 73 / stage diagnostic fixture"));
        assert(strstr(log, "[fixture] service state") && strstr(log, "[fixture] wait state"));
    } else if (kind == STALL) {
        assert(strstr(log, "[RED] watchdog: no progress") && strstr(log, "tick 73 / stage diagnostic fixture"));
        assert(strstr(log, "[fixture] service state") && strstr(log, "[fixture] wait state"));
    } else {
        char expected[40]; snprintf(expected, sizeof expected, "CRASH sig=%d", sig); assert(strstr(log, expected));
        assert(strstr(log, " pc=") && strstr(log, " lr="));
        if (kind == LOCKED) {
            assert(strstr(log, "stderr held for 1 s, no dump") && !strstr(log, "[fixture] service state"));
        } else {
            assert(strstr(log, "x28=") && strstr(log, " sp=") && strstr(log, "[dm] bt:") && strstr(log, "stack scan sp="));
            assert(strstr(log, "[fixture] service state"));
            if (kind == GUEST_FAULT) assert(strstr(log, "pc=diagnostic+0") && strstr(log, "addr=diagnostic+") && strstr(log, "x1=0x13579"));
        }
    }
}
int main(void) {
    alarm(20);
    const int cases[] = {DUMP, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGALRM, SIGABRT, GUEST_FAULT, LOCKED, STALL, DEADLINE};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) check(cases[i]);
    alarm(0); puts("PASS: 6 fatal signals, guest registers/module offsets, on-demand state, blocked stderr and both watchdogs");
    return 0;
}
