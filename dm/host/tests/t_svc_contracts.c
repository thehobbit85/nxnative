// Remaining thread SVC contracts on Mac and Android. Real thread/event objects
// and waits; only the assembly jump to a guest stack uses a C entry adapter.
#include "../hle_thread.c"
#include <assert.h>
#include <limits.h>
#include <unistd.h>

static uint32_t main_handle;
static _Atomic uint32_t word;
static _Atomic uint64_t wide;
typedef struct { uintptr_t addr; uint64_t value; unsigned type; uint32_t handle; _Atomic uint64_t result; } job_t;
static uint64_t call(unsigned num, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t r[32] = {a, b, c, d}; assert(svc_thread(num, r)); return r[0];
}
static void waiting_job(uint64_t arg) {
    job_t *job = (job_t *)(uintptr_t)arg;
    atomic_store(&job->result, call(0x34, job->addr, job->type, job->value, UINT64_MAX));
}
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)sp; ((void (*)(uint64_t))entry)(arg);
    (void)call(0x0a, 0, 0, 0, 0); abort();
}
static void start_job(job_t *job, unsigned prio) {
    atomic_store(&job->result, UINT64_MAX);
    uint64_t r[32] = {0, (uintptr_t)waiting_job, (uintptr_t)job, 0, prio, 1};
    assert(svc_thread(0x08, r) && !r[0]); job->handle = (uint32_t)r[1];
    assert(!call(0x09, job->handle, 0, 0, 0));
    for (;;) {
        pthread_mutex_lock(&G); thr_t *t = h_get(job->handle, O_THREAD); int queued = t && t->wkind == 3; pthread_mutex_unlock(&G);
        if (queued) break;
        assert(atomic_load(&job->result) == UINT64_MAX); sched_yield();
    }
}
static void finish_job(job_t *job) {
    assert(!call(0x18, 0, (uintptr_t)&job->handle, 1, UINT64_MAX));
    assert(atomic_load(&job->result) == 0);
    assert(!call(0x16, job->handle, 0, 0, 0));
}
static void wait_contracts(void) {
    const struct { unsigned type; uint64_t stored, wanted, result, after; } cases[] = {
        {0, 7, 7, 0xfa01, 7}, {0, 8, 7, 0xfa01, 8}, {0, 6, 7, 0xea01, 6},
        {0, UINT32_MAX, 0, 0xea01, UINT32_MAX}, // signed less-than
        {1, 7, 7, 0xfa01, 7}, {1, 6, 7, 0xea01, 5},
        {2, 7, 8, 0xfa01, 7}, {2, 7, 7, 0xea01, 7},
        {3, UINT64_C(0x100000007), 7, 0xfa01, UINT64_C(0x100000007)},
        {3, UINT64_C(0x100000007), UINT64_C(0x100000007), 0xea01, UINT64_C(0x100000007)},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        const typeof(*cases) *c = &cases[i]; atomic_store(&word, (uint32_t)c->stored); atomic_store(&wide, c->stored);
        uintptr_t addr = c->type == 3 ? (uintptr_t)&wide : (uintptr_t)&word;
        assert(call(0x34, addr, c->type, c->wanted, 0) == c->result);
        assert((c->type == 3 ? atomic_load(&wide) : atomic_load(&word)) == c->after);
    }
    for (unsigned type = 0; type < 4; type++) {
        atomic_store(&word, 7); atomic_store(&wide, UINT64_C(0x100000007));
        job_t job = {.addr = type == 3 ? (uintptr_t)&wide : (uintptr_t)&word, .type = type,
                     .value = type == 3 ? UINT64_C(0x100000007) : type == 2 ? 7 : 8};
        start_job(&job, 46);
        if (type == 1) assert(atomic_load(&word) == 6);
        assert(!call(0x35, job.addr, 0, 0, 1)); finish_job(&job);
    }
    puts("WaitForAddress: signed/64-bit comparisons, decrement, mismatch, timeout and real wake passed");
}
static void signal_contracts(void) {
    const int counts[] = {-1, 0, 1, 2, 3, 4};
    // Independent protocol table for SignalAndModifyByWaitingCountIfEqual.
    const int deltas[][6] = {{1, 1, 1, 1, 1, 1}, {-1, -1, -1, -1, -1, -1}, {-1, -1, 0, 0, -1, -1}};
    const unsigned sizes[] = {0, 1, 3};
    for (unsigned type = 0; type < 3; type++) for (unsigned row = 0; row < 3; row++) for (unsigned col = 0; col < 6; col++) {
        job_t jobs[3] = {0}; unsigned n = sizes[row]; atomic_store(&word, 100);
        for (unsigned i = 0; i < n; i++) {
            jobs[i] = (job_t){.addr = (uintptr_t)&word, .type = 2, .value = 100};
            start_job(&jobs[i], i == 0 ? 50 : 45);
        }
        if (type) {
            assert(call(0x35, (uintptr_t)&word, type, 101, counts[col]) == 0xfa01);
            pthread_mutex_lock(&G);
            for (unsigned i = 0; i < n; i++) assert(((thr_t *)h_get(jobs[i].handle, O_THREAD))->wkind == 3);
            pthread_mutex_unlock(&G); assert(atomic_load(&word) == 100);
        }
        assert(!call(0x35, (uintptr_t)&word, type, 100, counts[col]));
        unsigned woke = counts[col] <= 0 || (unsigned)counts[col] > n ? n : (unsigned)counts[col];
        const unsigned order[] = {1, 2, 0};
        pthread_mutex_lock(&G);
        for (unsigned i = 0; i < n; i++) {
            unsigned k = n == 1 ? 0 : order[i]; thr_t *t = h_get(jobs[k].handle, O_THREAD);
            assert(t && (i < woke ? t->wkind == 0 : t->wkind == 3));
        }
        pthread_mutex_unlock(&G);
        assert(atomic_load(&word) == (uint32_t)(100 + (type == 2 ? deltas[row][col] : type == 1 ? 1 : 0)));
        assert(!call(0x35, (uintptr_t)&word, 0, 0, 0));
        for (unsigned i = 0; i < n; i++) finish_job(&jobs[i]);
    }
    for (unsigned type = 1; type <= 2; type++) {
        atomic_store(&word, 0x7fffffff);
        assert(!call(0x35, (uintptr_t)&word, type, 0x7fffffff, 0));
        assert(atomic_load(&word) == 0x80000000);
    }
    atomic_store(&word, 0x80000000);
    job_t boundary = {.addr = (uintptr_t)&word, .type = 2, .value = 0x80000000};
    start_job(&boundary, 46); assert(!call(0x35, (uintptr_t)&word, 2, 0x80000000, 0));
    assert(atomic_load(&word) == 0x7fffffff); finish_job(&boundary);
    puts("SignalToAddress: 54 type/count/queue cases, mismatch and priority/FIFO selection passed");
}
static void event_contracts(void) {
    uint64_t r[32] = {0}; assert(svc_thread(0x45, r) && !r[0]); uint32_t wh = r[1], rh = r[2];
    assert(call(0x17, rh, 0, 0, 0) == 0xfa01);
    assert(call(0x17, wh, 0, 0, 0) == 0xe401);
    assert(call(0x11, rh, 0, 0, 0) == 0xe401);
    assert(call(0x18, 0, (uintptr_t)&wh, 1, 0) == 0xe401);
    for (unsigned k = 0; k < 2; k++) {
        assert(!call(0x11, wh, 0, 0, 0)); assert(!call(0x18, 0, (uintptr_t)&rh, 1, 0));
        assert(!call(0x12, k ? rh : wh, 0, 0, 0)); assert(call(0x18, 0, (uintptr_t)&rh, 1, 0) == 0xea01);
        assert(!call(0x12, k ? rh : wh, 0, 0, 0)); // ClearEvent on a quiet event succeeds.
    }
    assert(!call(0x11, wh, 0, 0, 0)); assert(!call(0x17, rh, 0, 0, 0));
    assert(call(0x18, 0, (uintptr_t)&rh, 1, 0) == 0xea01);
    assert(call(0x11, main_handle, 0, 0, 0) == 0xe401);
    assert(call(0x12, main_handle, 0, 0, 0) == 0xe401);
    assert(call(0x17, main_handle, 0, 0, 0) == 0xfa01);
    assert(!call(0x16, wh, 0, 0, 0)); assert(!call(0x16, rh, 0, 0, 0));
    const unsigned ops[] = {0x11, 0x12, 0x17};
    for (unsigned i = 0; i < 3; i++) {
        assert(call(ops[i], wh, 0, 0, 0) == 0xe401); assert(call(ops[i], 0, 0, 0, 0) == 0xe401);
    }
    puts("Event SVCs: wrong ends, reset/clear distinctions, quiet/signalled and invalid/thread handles passed");
}
static void invalid_threads(void) {
    const unsigned simple[] = {0x09, 0x0d, 0x0f, 0x19, 0x32};
    for (unsigned i = 0; i < sizeof simple / sizeof *simple; i++) assert(call(simple[i], 0, 0, 0, 0) == 0xe401);
    uint64_t r[32] = {0xa5, 0}; assert(svc_thread(0x0c, r) && r[0] == 0xe401 && !r[1]);
    r[0] = 0xa5; r[1] = 0xa5; r[2] = 0; assert(svc_thread(0x0e, r) && r[0] == 0xe401 && !r[1] && !r[2]);
    r[0] = 0xa5; r[1] = 0; assert(svc_thread(0x25, r) && r[0] == 0xe401 && !r[1]);
    uint8_t context[1024]; memset(context, 0xa5, sizeof context);
    assert(call(0x33, (uintptr_t)context, 0, 0, 0) == 0xe401);
    assert(call(0x33, (uintptr_t)context, main_handle, 0, 0) == 0xfa01);
    for (unsigned i = 0; i < sizeof context; i++) assert(context[i] == 0xa5);
    assert(call(0x32, main_handle, 1, 0, 0) == 0xfa01);
    puts("Thread SVCs: invalid-handle outputs and context/activity state errors passed");
}
int main(void) {
    alarm(30); unsetenv("DM_HITCH_DUMP"); setenv("DM_PREEMPT", "0", 1); main_handle = svc_thread_main_init();
    int handles, live, objects; thr_counts(&handles, &live, &objects);
    wait_contracts(); signal_contracts(); event_contracts(); invalid_threads();
    int h, l, o; thr_counts(&h, &l, &o); assert(h == handles && l == live && o == objects);
    puts("Remaining thread SVC contracts passed with no leaked handles or thread objects"); return 0;
}
