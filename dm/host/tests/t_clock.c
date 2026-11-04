// Shared clock/calendar and real SVC deadlines under controlled clock producers.
// pthread waits, the guest clock and service implementations remain production code.
#include "../hle.h"
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

static _Atomic uint64_t mono = 10000000000ull, real = 1700000000123456789ull;
static int test_clock_gettime(clockid_t id, struct timespec *t) {
    assert(id == CLOCK_MONOTONIC || id == CLOCK_REALTIME);
    uint64_t n = atomic_load(id == CLOCK_MONOTONIC ? &mono : &real);
    t->tv_sec = n / 1000000000; t->tv_nsec = n % 1000000000; return 0;
}
static time_t test_time(time_t *out) { time_t t = atomic_load(&real) / 1000000000; if (out) *out = t; return t; }
#ifdef __APPLE__
static uint64_t test_mach_absolute_time(void) { return atomic_load(&mono); }
static kern_return_t test_mach_timebase_info(mach_timebase_info_t t) { t->numer = t->denom = 1; return 0; }
#define mach_absolute_time test_mach_absolute_time
#define mach_timebase_info test_mach_timebase_info
#endif
static _Atomic unsigned waits, sleeps;
static int observed_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    atomic_fetch_add(&waits, 1);
    return pthread_cond_wait(c, m);
}
static int observed_sleep(const struct timespec *t, struct timespec *left) {
    atomic_fetch_add(&sleeps, 1);
    return nanosleep(t, left);
}
#define pthread_cond_wait observed_wait
#define clock_gettime test_clock_gettime
#define time test_time
#include "../hle_os.c"
#include "../hle_time.c"
#undef time
#undef clock_gettime
#ifdef __APPLE__
#undef mach_absolute_time
#undef mach_timebase_info
#endif
#define nanosleep observed_sleep
#include "../hle_thread.c"
#undef nanosleep
#undef pthread_cond_wait

module_t *g_mod; int g_nmod;
void *module_sym(module_t *m, const char *n) { (void)m; (void)n; return NULL; }
static int ounce;
int dm_sdk_ounce(void) { return ounce; }
void crash_watch_frame(void) {}

// Acquire the production wait's mutex after its entry was observed: this proves
// the waiter has released it, without a guessed host-time delay.
static void await_wait(unsigned n, pthread_mutex_t *m) {
    while (atomic_load(&waits) < n) sched_yield();
    pthread_mutex_lock(m); pthread_mutex_unlock(m);
}
static int64_t load64(const void *p) { int64_t v; memcpy(&v, p, 8); return v; }
static void calendar(void) {
    _Static_assert(sizeof(CalendarTime) == 8, "calendar ABI");
    _Static_assert(sizeof(CalendarAdditionalInfo) == 24, "additional info ABI");
    struct sample { const char *zone; int64_t epoch; int y, m, d, h, min, sec, wday, yday, dst, offset; const char *abbr; };
    static const struct sample samples[] = {
        {"UTC", 1709251198, 2024,2,29,23,59,58,4,59,0,0,"UTC"},
        {"America/Los_Angeles",1710064799,2024,3,10,1,59,59,0,69,0,-28800,"PST"},
        {"America/Los_Angeles",1710064800,2024,3,10,3,0,0,0,69,1,-25200,"PDT"},
        {"America/Los_Angeles",1730624399,2024,11,3,1,59,59,0,307,1,-25200,"PDT"},
        {"America/Los_Angeles",1730624400,2024,11,3,1,0,0,0,307,0,-28800,"PST"},
    };
    uint8_t rule[128]; memset(rule, 0xa5, sizeof rule);
    assert(h_time_LoadRule(rule, "UTC") == 0);
    for (size_t i = 0; i < sizeof rule; i++) assert(rule[i] == 0xa5);
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++) {
        const struct sample *s = &samples[i]; setenv("TZ", s->zone, 1); tzset();
        struct { uint64_t pre; CalendarTime ct; CalendarAdditionalInfo ai; uint64_t post; } g;
        memset(&g, 0xa5, sizeof g);
        assert(h_time_ToCalendarTime(&g.ct, &g.ai, &s->epoch) == 0);
        assert(g.pre == 0xa5a5a5a5a5a5a5a5ull && g.post == g.pre);
        assert(g.ct.year == s->y && g.ct.month == s->m && g.ct.day == s->d);
        assert(g.ct.hour == s->h && g.ct.minute == s->min && g.ct.second == s->sec && g.ct.pad == 0);
        assert(g.ai.wday == (unsigned)s->wday && g.ai.yday == (unsigned)s->yday);
        assert(g.ai.dst == (unsigned)s->dst && g.ai.offset == s->offset && !strcmp(g.ai.tzname, s->abbr));
        CalendarTime ct; CalendarAdditionalInfo ai;
        assert(h_time_ToCalendarTimeRule(&ct, &ai, &s->epoch, rule) == 0);
        assert(!memcmp(&ct, &g.ct, sizeof ct) && !memcmp(&ai, &g.ai, sizeof ai));
        assert(h_time_ToCalendarTime(NULL, NULL, &s->epoch) == 0);
        struct tm tm; assert(h_localtime_r(&s->epoch, &tm) == &tm);
        assert(tm.tm_hour == s->h && tm.tm_isdst == s->dst && tm.tm_gmtoff == s->offset);
        assert(h_mktime(&tm) == s->epoch); // explicit DST disambiguates the repeated autumn hour
        assert(h_localtime(&s->epoch)->tm_min == s->min);
        if (i < 3) { // SDK conversion has tm_isdst=-1; only unambiguous local times here
            int count = -1; int64_t out[2] = {-77, -88};
            assert(h_time_ToPosixTime(&count, out, 2, &ct) == 0 && count == 1 && out[0] == s->epoch && out[1] == -88);
            out[0] = -77;
            assert(h_time_ToPosixTimeRule(&count, out, 1, &ct, rule) == 0 && count == 1 && out[0] == s->epoch);
            out[0] = -77;
            assert(h_time_ToPosixTime(&count, out, 0, &ct) == 0 && count == 0 && out[0] == -77);
        }
    }
    CalendarTime leap = {2024,2,29,23,59,58,0};
    assert(h_time_ToPosixUtc(&leap) == 1709251198); // independent of the current host zone
    puts("PASS: UTC leap day, DST boundaries, calendar ABI bounds and explicit-rule compatibility");
}
static void time_services(int fixed) {
    assert(h_time_Initialize() == 0 && h_time_IsInitialized() == 1 && h_time_NetAccurate() == 0);
    int64_t t = 0, v = fixed ? 1234567890 : 1700000000;
    assert(h_time_GetCurrentTime(&t) == 0 && t == v);
    assert(h_time_AdjGetCurrentTime(&t) == 0 && t == v);
    assert(h_time_now() == v && h_time_to_time_t(&v) == v);
    int64_t ts[3] = {-1,-1,-77};
    assert(h_time_StdTimespec(ts) == 0 && ts[0] == v && ts[1] == (fixed ? 0 : 123456789) && ts[2] == -77);
    assert(h_time_AdjTimespec(ts) == 0 && ts[0] == v && ts[1] == (fixed ? 0 : 123456789) && ts[2] == -77);
    atomic_store(&mono, 10000000333ull);
    assert(h_time_MonoTimespec(ts) == 0 && ts[0] == 10 && ts[1] == 333 && ts[2] == -77);
    uint8_t tp[32], other[24]; memset(tp, 0xa5, sizeof tp);
    assert(h_time_SteadyNow(tp) == 0 && load64(tp) == 10 && !memcmp(tp + 8, "dmhost-steady-01", 16));
    for (int i = 24; i < 32; i++) assert(tp[i] == 0xa5);
    assert(h_time_SteadyNowFn(other) == 0 && !memcmp(tp, other, 24));
    uint8_t a[32], b[32]; assert(h_time_GetContext(a) == 0 && load64(a) == v - 10 && load64(a + 8) == 10);
    atomic_fetch_add(&mono, 5000000000ull); atomic_fetch_add(&real, 5000000000ull);
    assert(h_time_GetContext(b) == 0 && !memcmp(a, b, 32)); // ordinary passage of time is not a clock reset
    atomic_fetch_add(&real, 1000000000ull);
    assert(h_time_GetContext(b) == 0 && !memcmp(a, b, 32)); // one-second threshold
    atomic_fetch_add(&real, 1000000000ull);
    assert(h_time_GetContext(b) == 0);
    if (fixed) assert(!memcmp(a, b, 32));
    else assert(load64(b) == v - 8 && load64(b + 8) == 15 && !memcmp(a + 16, b + 16, 16));
    puts("PASS: user/adjustable/steady times, ABI bounds and fixed-time/context-drift contracts");
}
static _Atomic int read_done;
static void *background_read(void *unused) { (void)unused; guest_clock_read_wait(); atomic_store(&read_done, 1); return NULL; }
static void freeze_clock(void) {
    atomic_store(&mono, 10000000000ull);
    assert(now_ns() == 10000000000ull && guest_ns() == 10000000000ull);
    assert(guest_ticks() == 192000000); ounce = 1; assert(guest_ticks() == 312500000); ounce = 0;
    guest_clock_freeze_for(2, 1); atomic_fetch_add(&mono, 5000000000ull);
    guest_clock_read_wait(); assert(guest_ns() == 10000000000ull); // keyboard freeze does not block reads
    guest_clock_freeze_for(1, 1); guest_clock_freeze_for(2, 0);
    unsigned n = atomic_load(&waits) + 1; pthread_t p; assert(!pthread_create(&p, NULL, background_read, NULL));
    await_wait(n, &g_gfz_mu); assert(!atomic_load(&read_done));
    dm_host_work = 1; guest_clock_read_wait(); dm_host_work = 0; // submit/present may read the frozen clock
    guest_clock_freeze(1); atomic_fetch_add(&mono, 5000000000ull);
    guest_clock_freeze_for(1, 0); assert(!pthread_join(p, NULL));
    assert(atomic_load(&read_done) && guest_ns() == 10000000000ull); // another freeze reason still holds
    guest_clock_freeze(0); assert(guest_ns() == 10000000000ull);
    atomic_fetch_add(&mono, 1000000000ull); assert(guest_ns() == 11000000000ull);
    assert(guest_ticks() == 211200000); ounce = 1; assert(guest_ticks() == 343750000); ounce = 0;
    puts("PASS: counter units, nested freezes, background read blocking and continuous resume");
}
static unsigned next_wait; static uint32_t event_w, event_r, main_handle;
static _Atomic uint32_t lock_word, condition_key;
static int signal_case;
static void *drive_deadline(void *unused) {
    (void)unused; await_wait(next_wait++, &G);
    uint64_t start = guest_ns();
    guest_clock_freeze_for(1, 1);
    for (int i = 0; i < 4; i++) {
        atomic_fetch_add(&mono, 1000000000ull); dm_guest_frame_tick();
        assert(guest_ns() == start);
        dm_guest_clock_advanced(); await_wait(next_wait++, &G); // spurious wake cannot expire a frozen deadline
    }
    guest_clock_freeze_for(1, 0);
    dm_guest_frame_tick(); await_wait(next_wait++, &G); assert(guest_ns() == start + 1000);
    dm_guest_frame_tick(); await_wait(next_wait++, &G); assert(guest_ns() == start + 2000);
    if (signal_case) { uint64_t r[32] = {event_w}; assert(svc_thread(0x11, r) && r[0] == 0); }
    else dm_guest_frame_tick(); // exactly the 3000 ns deadline
    return NULL;
}
static void *drive_sleep(void *unused) {
    (void)unused;
    unsigned n = 1;
    while (atomic_load(&sleeps) < n) sched_yield();
    guest_clock_freeze(1); uint64_t start = guest_ns();
    for (int i = 0; i < 4; i++) { dm_guest_frame_tick(); assert(guest_ns() == start); }
    guest_clock_freeze(0);
    for (int i = 0; i < 2; i++) {
        n = atomic_load(&sleeps) + 1; dm_guest_frame_tick();
        while (atomic_load(&sleeps) < n) sched_yield();
    }
    dm_guest_frame_tick(); return NULL;
}
static void deadlines(void) {
    main_handle = svc_thread_main_init();
    assert(!dm_detclock_enabled()); uint64_t before = guest_ns();
    atomic_fetch_add(&mono, 1000000); assert(guest_ns() == before + 1000000); // boot still uses wall time
    before = guest_ns(); dm_guest_frame_tick(); assert(dm_detclock_enabled() && guest_ns() == before);
    atomic_fetch_add(&mono, 1000000000); assert(guest_ns() == before);
    uint64_t r[32] = {0}; assert(svc_thread(0x45, r) && r[0] == 0); event_w = r[1]; event_r = r[2];
    for (int i = 0; i < 3; i++) {
        signal_case = i == 1; next_wait = atomic_load(&waits) + 1;
        pthread_t p; assert(!pthread_create(&p, NULL, drive_deadline, NULL));
        uint64_t start = guest_ns();
        if (i < 2) {
            uint64_t a[32] = {0, (uint64_t)&event_r, 1, 3000}; assert(svc_thread(0x18, a));
            assert(a[0] == (signal_case ? 0 : 0xea01));
        } else {
            atomic_store(&lock_word, main_handle);
            uint64_t a[32] = {(uint64_t)&lock_word, (uint64_t)&condition_key, main_handle, 3000};
            assert(svc_thread(0x1c, a) && a[0] == 0xea01);
            // Horizon leaves the key hint set on timeout; Signal clears it once
            // no waiters remain (Mesosphere KConditionVariable::CancelWait/Signal).
            assert(atomic_load(&condition_key) == 1 && atomic_load(&lock_word) == 0);
            uint64_t clear[32] = {(uint64_t)&condition_key, 1};
            assert(svc_thread(0x1d, clear) && clear[0] == 0 && atomic_load(&condition_key) == 0);
        }
        assert(!pthread_join(p, NULL)); assert(guest_ns() == start + (signal_case ? 2000 : 3000));
        assert(g_wait == NULL);
    }
    uint64_t start = guest_ns(); pthread_t p; assert(!pthread_create(&p, NULL, drive_sleep, NULL));
    uint64_t sleep_regs[32] = {3000}; assert(svc_thread(0x0b, sleep_regs));
    assert(!pthread_join(p, NULL) && guest_ns() == start + 3000);
    r[0] = event_w; assert(svc_thread(0x16, r) && r[0] == 0);
    r[0] = event_r; assert(svc_thread(0x16, r) && r[0] == 0);
    puts("PASS: first-present continuity, frozen frames and exact event/condition/sleep deadlines");
}
int main(int argc, char **argv) {
    alarm(30); assert(argc == 2);
    unsetenv("DM_FIXED_TIME"); unsetenv("DM_DETCLOCK");
    if (!strcmp(argv[1], "det")) { setenv("DM_DETCLOCK", "1000", 1); freeze_clock(); deadlines(); }
    else {
        int fixed = !strcmp(argv[1], "fixed"); assert(fixed || !strcmp(argv[1], "wall"));
        if (fixed) setenv("DM_FIXED_TIME", "1234567890", 1);
        calendar(); time_services(fixed); freeze_clock();
    }
    return 0;
}
