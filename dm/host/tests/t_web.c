// Production session handling with a controlled asynchronous frontend boundary.
// Real WebView/SDK event integration is exercised by Android's web_smoke.py.
#include "../hle.h"
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <unistd.h>
static int callback_lock(pthread_mutex_t *mu);
#define pthread_mutex_lock callback_lock
#define DM_TEST_WEB_FRONT 1
#include "../hle_web.c"
#undef pthread_mutex_lock
static _Thread_local int gate;
static _Atomic int entered, released;
static uint64_t session;
static int held, signalled;
static int callback_lock(pthread_mutex_t *mu) {
    if (gate) {
        gate = 0; atomic_store(&entered, 1);
        while (!atomic_load(&released)) sched_yield();
    }
    return pthread_mutex_lock(mu);
}
void dm_input_hold(int on) { held = on; }
void hle_bind_system_event(void *event, int mode) { assert(event && !mode); }
void hle_clear_system_event(void *event) { assert(event); signalled = 0; }
void hle_signal_system_event(void *event) { assert(event); signalled++; }
int front_web_open(const char *dir, const char *path, const char *callback, int hidden, const dm_web_opts *opts) {
    assert(dir && !strcmp(path, "index.html") && !strcmp(callback, "http://localhost/") && !hidden);
    assert(opts->session && opts->session != session); session = opts->session; return 0;
}
void front_web_close(void) {}
void front_web_show(void) {}
void front_web_send(const char *s, size_t n) { (void)s; (void)n; }
void front_web_shot(const char *path) { (void)path; }
struct callback { uint64_t token; int kind; };
static void *report_old(void *opaque) {
    struct callback *call = opaque; gate = 1;
#ifdef DM_TEST_WEB_OLD_CALLBACK
    // Negative control: Android's original outside-the-mutex check. The
    // callback is paused just after this check, before the session lock.
    if (call->token != session) return NULL;
    uint64_t token = 0;
#else
    uint64_t token = call->token;
#endif
    if (call->kind == 0) dm_web_message_for(token, "old", 3);
    if (call->kind == 1) dm_web_ended_for(token, 3, "http://localhost/old");
    if (call->kind == 2) dm_web_failed_for(token, "old page failed");
    return NULL;
}
int main(void) {
    char dir[64] = "/tmp/nx-web-XXXXXX";
#ifdef __ANDROID__
    strcpy(dir, "/data/local/tmp/nx-web-XXXXXX");
#endif
    assert(mkdtemp(dir)); char path[128]; snprintf(path, sizeof path, "%s/index.html", dir);
    FILE *f = fopen(path, "w"); assert(f); fputs("<!doctype html>", f); fclose(f); setenv("DM_HTML", dir, 1);
    _Alignas(16) uint8_t arg[0x2000] = {0};
    *(uint16_t *)arg = 1; *(uint16_t *)(arg + 8) = 6; *(uint16_t *)(arg + 10) = 11; memcpy(arg + 16, "index.html", 11);
    for (int kind = 0; kind < 3; kind++) {
        assert(!h_webs_Start(NULL, NULL, arg) && held && !signalled);
        struct callback call = {session, kind}; pthread_t thread;
        atomic_store(&entered, 0); atomic_store(&released, 0);
        assert(!pthread_create(&thread, NULL, report_old, &call));
        while (!atomic_load(&entered)) sched_yield();
        assert(!h_webs_Start(NULL, NULL, arg)); // old callback has not acquired g_web_mu
        atomic_store(&released, 1); assert(!pthread_join(thread, NULL));
        char message[8] = {0}; size_t n = 0;
        if (!held || signalled || h_webs_Recv(NULL, &n, message, sizeof message)) {
            puts("FAIL: old callback changed the replacement session"); unlink(path); rmdir(dir); return 1;
        }
        dm_web_message_for(session, "new", 3);
        assert(h_webs_Recv(NULL, &n, message, sizeof message) && n == 3 && !strcmp(message, "new"));
        h_webs_ReqExit(NULL); assert(!held && signalled == 1);
        uint64_t result[0x1010 / 8] = {0}; assert(!h_webs_Wait(NULL, result) && result[0] == 2 && !signalled);
        assert(!h_webs_Needs(NULL));
    }
    unlink(path); assert(!rmdir(dir));
    puts("web: message/end/failure callbacks blocked before the lock cannot change a replacement session"); return 0;
}
