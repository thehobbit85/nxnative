// Exercise the production termination handler and deadline in a child process.
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void dm_term_install(void);
static int report, stall, from_signal;
static pthread_t caller;
static void step(char tag) { assert(write(report, &tag, 1) == 1); }
uint64_t now_ns(void) {
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void hlecount_flush(void) {
    if (from_signal) assert(!pthread_equal(pthread_self(), caller));
    step('H');
}
void dm_rec_finish(void) { step('R'); dm_host_shutdown("already stopping"); }
void dm_cheats_exit(void) { step('C'); }
void dm_audio_suspend(int on) { assert(on == 1); step('A'); }
void dm_gfx_suspend(int on) {
    assert(on == 1);
    step('G');
    if (stall) for (;;) pause();
}

static void handoff(void *arg) { assert(arg == &report); step('P'); }

static void check(int by_signal, int block_graphics, int next_program) {
    int p[2], status;
    assert(!pipe(p));
    uint64_t start = now_ns();
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(p[0]);
        report = p[1];
        stall = block_graphics;
        alarm(8);
        caller = pthread_self();
        from_signal = by_signal;
        dm_term_install();
        if (by_signal) {
#ifdef __ANDROID__
            // Deliver to this activity thread to catch running the hooks in the handler.
            assert(!raise(SIGTERM));
#else
            // Darwin dispatch sources receive process-directed signals.
            assert(!kill(getpid(), SIGTERM));
#endif
            for (;;) pause();
        }
        if (next_program) dm_host_shutdown_then("next program", handoff, &report);
        else dm_host_shutdown("activity destroyed");
        _exit(1);
    }
    close(p[1]);
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char steps[10];
    int count = next_program ? 6 : 5;
    assert(read(p[0], steps, sizeof steps) == count && !memcmp(steps, "HRCAGP", count));
    close(p[0]);
    if (block_graphics) assert(now_ns() - start >= 1500000000);
}

int main(void) {
    check(1, 0, 0);
    check(0, 0, 0);
    check(1, 1, 0);
    check(0, 0, 1);
    check(0, 1, 1);
    puts("SIGTERM, direct shutdown, ordered hooks, deadline and program handoff passed");
}
