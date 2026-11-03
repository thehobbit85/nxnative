// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#else
#include <semaphore.h>
#include <time.h>
#endif
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
void hlecount_flush(void); void dm_rec_finish(void);
#ifndef __APPLE__
// The drain can block on a worker that also holds stderr. The deadline must
// be independent of that worker, including its log locks.
static void (*shutdown_then)(void *);
static void *shutdown_arg;
static void *shutdown_deadline(void *unused) {
    struct timespec delay = {1, 500000000};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
    if (shutdown_then) shutdown_then(shutdown_arg);
    _exit(0);
}
#endif

void dm_host_shutdown_then(const char *why, void (*then)(void *), void *arg) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
#ifndef __APPLE__
    shutdown_then = then; shutdown_arg = arg;
    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, shutdown_deadline, NULL)) { if (then) then(arg); _exit(0); }
    pthread_detach(watchdog);
#endif
    uint64_t t0 = now_ns(); LOG("received %s: graceful exit", why);
#ifdef __APPLE__
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("[RED] graceful exit not done in 1.5 seconds, exiting at once"); if (then) then(arg); _exit(0); });
#endif
    hlecount_flush();                               // DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_rec_finish();                                // DM_REC: the held state and the buffered steps (hle_hid.c)
    dm_cheats_exit();                               // a clean exit: no safe mode for the cheats at the next launch (atexit does not run either)
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    if (then) then(arg);
    _exit(0);
}
void dm_host_shutdown(const char *why) { dm_host_shutdown_then(why, NULL, NULL); }
#ifdef __APPLE__
void dm_term_install(void) {
    signal(SIGTERM, SIG_IGN);                       // do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
}
#else
// An Android activity already has framework threads when the host starts, so
// blocking SIGTERM only on the installing thread cannot route it to sigwait.
static sem_t term_event;
static void term_signal(int sig) {
    int saved_errno = errno;
    sem_post(&term_event);
    errno = saved_errno;
}

static void *wait_for_term(void *unused) {
    while (sem_wait(&term_event)) {
        if (errno != EINTR) die("waiting for SIGTERM: %s", strerror(errno));
    }
    dm_host_shutdown("SIGTERM");
    return NULL;
}

void dm_term_install(void) {
    if (sem_init(&term_event, 0, 0)) die("SIGTERM semaphore: %s", strerror(errno));
    pthread_t worker;
    int err = pthread_create(&worker, NULL, wait_for_term, NULL);
    if (err) die("SIGTERM thread: %s", strerror(err));
    pthread_detach(worker);
    struct sigaction sa = {.sa_handler = term_signal, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL)) die("installing SIGTERM handler: %s", strerror(errno));
}
#endif
