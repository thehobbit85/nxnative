// Managed Android write watches. Fresh shared backing is installed by the
// allocation owner, before publication. No live page is copied or remapped.
// Signal handlers lease the alias, execute a native-width store, and enqueue a
// bounded record. Logging/symbol lookup run on a separate host thread.
#include "dm.h"
#include "watch_android.h"
#include "watch_store.h"
#include "host_memory.h"
#include <dlfcn.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <time.h>

static dm_watch_page g_watch_page = {.fd = -1};
static pthread_mutex_t g_watch_owner = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_watch_once = PTHREAD_ONCE_INIT;
static uintptr_t g_watch_at, g_watch_offset;
static size_t g_watch_len, g_watch_pg;
static int g_watch_requested, g_watch_relative, g_watch_kind, g_watch_installed, g_watch_foreign;
static uint64_t g_watch_dczid;
// 0 = armed, 1 = opening for an unsupported write, 2 = disabled, 3 = failure.
static _Atomic int g_watch_disarmed;
enum { WATCH_STORE, WATCH_INSTRUCTION, WATCH_HOST_WRITE };
enum { WATCH_RECORDS = 64, WATCH_FRAMES = 8 };
typedef struct {
    uint64_t sequence, generation, ns, at, address, pc, lr, old_value, new_value, frames[WATCH_FRAMES];
    uint32_t thread, tid, instruction, size, reason, nframes;
} watch_record;
static struct { _Atomic unsigned state; watch_record record; } g_watch_records[WATCH_RECORDS];
static _Atomic uint64_t g_watch_sequence, g_watch_dropped;
static int g_watch_wake = -1;

static void watch_configure(void) {
    const char *e = getenv("DM_PROBE_WATCH"); if (!e) return;
    g_watch_relative = !strncmp(e, "heap+", 5); if (g_watch_relative) e += 5;
    char *end; errno = 0; uint64_t at = strtoull(e, &end, 16);
    if (errno || end == e || *e == '-') die("DM_PROBE_WATCH: invalid address");
    uint64_t n = 8;
    if (*end == '+') {
        e = end + 1; errno = 0; n = strtoull(e, &end, 16);
        if (errno || end == e || *e == '-') die("DM_PROBE_WATCH: invalid length");
    }
    g_watch_pg = (size_t)getpagesize();
    if (*end || (!at && !g_watch_relative) || !n || n > g_watch_pg ||
        (at & (g_watch_pg - 1)) > g_watch_pg - n || at > UINTPTR_MAX - n)
        die("DM_PROBE_WATCH format: [heap+]<address>[+<length>] (hex, within one host page)");
    g_watch_offset = at; g_watch_len = (size_t)n;
    if (!g_watch_relative) g_watch_at = at;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(g_watch_dczid));
    g_watch_requested = 1;
}
static int watch_overlap(uintptr_t a, size_t n, uintptr_t b, size_t m) {
    return n && m && (a <= b ? b - a < n : a - b < m);
}
// Caller has a lease. The alias remains live, even if retirement has closed
// admission and is waiting for this handler to finish.
static uint64_t watch_value(void) {
    uint64_t v = 0; size_t n = g_watch_len < 8 ? g_watch_len : 8;
    memcpy(&v, g_watch_page.alias + (g_watch_at - (uintptr_t)g_watch_page.guest), n); return v;
}
static void watch_enqueue(watch_record *r, uintptr_t fp) {
    r->sequence = atomic_fetch_add_explicit(&g_watch_sequence, 1, memory_order_relaxed) + 1;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    r->ns = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    r->generation = g_watch_page.generation; r->at = g_watch_at;
    uint32_t thr_cur_handle(void); r->thread = thr_cur_handle(); r->tid = (uint32_t)gettid();
    while (r->nframes < WATCH_FRAMES && fp && !(fp & 15)) {
        uint64_t words[2]; if (!dm_host_read(fp, words, sizeof words)) break;
        r->frames[r->nframes++] = words[1];
        if (words[0] <= fp) break;
        fp = words[0];
    }
    unsigned start = (unsigned)r->sequence % WATCH_RECORDS;
    for (unsigned k = 0; k < WATCH_RECORDS; k++) {
        unsigned i = (start + k) % WATCH_RECORDS, empty = 0;
        if (!atomic_compare_exchange_strong_explicit(&g_watch_records[i].state, &empty, 1, memory_order_acquire, memory_order_relaxed)) continue;
        g_watch_records[i].record = *r;
        atomic_store_explicit(&g_watch_records[i].state, 2, memory_order_release);
        goto wake;
    }
    atomic_fetch_add_explicit(&g_watch_dropped, 1, memory_order_relaxed);
wake:;
    uint64_t one = 1; if (write(g_watch_wake, &one, sizeof one) < 0) {} // nonblocking eventfd, no handler locks
}
static void watch_where(const char *label, uint64_t address) {
    module_t *m = module_of((void *)(uintptr_t)address); Dl_info di;
    if (m) fprintf(stderr, " %s=%s+%#llx", label, m->name, (unsigned long long)(address - (uintptr_t)m->base));
    else if (dladdr((void *)(uintptr_t)address, &di) && di.dli_sname)
        fprintf(stderr, " %s=%s+%#llx", label, di.dli_sname, (unsigned long long)(address - (uintptr_t)di.dli_saddr));
    else fprintf(stderr, " %s=%#llx", label, (unsigned long long)address);
}
static void *watch_logger(void *arg) {
    (void)arg; uint64_t reported = 0;
    for (;;) {
        struct pollfd pfd = {.fd = g_watch_wake, .events = POLLIN};
        if (poll(&pfd, 1, -1) < 0) continue;
        uint64_t wake; if (read(g_watch_wake, &wake, sizeof wake) < 0) continue;
        for (unsigned i = 0; i < WATCH_RECORDS; i++) {
            if (atomic_load_explicit(&g_watch_records[i].state, memory_order_acquire) != 2) continue;
            watch_record r = g_watch_records[i].record;
            atomic_store_explicit(&g_watch_records[i].state, 0, memory_order_release);
            flockfile(stderr);
            fprintf(stderr, "[dm] WATCH %#llx seq=%llu gen=%llu time=%.6f T=%x tid=%u", (unsigned long long)r.at,
                    (unsigned long long)r.sequence, (unsigned long long)r.generation, r.ns / 1e9, r.thread, r.tid);
            if (r.reason == WATCH_STORE) fprintf(stderr, " store of %u bytes at %#llx: %#llx -> %#llx", r.size,
                (unsigned long long)r.address, (unsigned long long)r.old_value, (unsigned long long)r.new_value);
            else fprintf(stderr, " disabled for this allocation: %s instruction=%08x",
                         r.reason == WATCH_HOST_WRITE ? "host write preparation" : "unsupported or crossing store", r.instruction);
            watch_where("pc", r.pc); watch_where("lr", r.lr); fputc('\n', stderr);
            for (unsigned j = 0; j < r.nframes; j++) watch_where("frame", r.frames[j]);
            if (r.nframes) fputc('\n', stderr);
            funlockfile(stderr);
        }
        uint64_t dropped = atomic_load(&g_watch_dropped);
        if (dropped != reported) { LOG("WATCH dropped %llu records (logger queue full; stores still executed)", (unsigned long long)(dropped - reported)); reported = dropped; }
    }
    return NULL;
}
// Lease held. The ordinary instruction/kernel write will run after this returns.
// This generation stays disabled; no timer can re-protect or resurrect it.
static int watch_disarm(unsigned reason, uint32_t instruction, ucontext_t *uc) {
    int expected = 0;
    int first = atomic_compare_exchange_strong(&g_watch_disarmed, &expected, 1);
    if (!first && expected >= 2) return expected == 2;
    // Another caller may be descheduled in its mprotect. Help complete the same
    // transition instead of waiting inside a signal handler. Every helper holds
    // a lease, so retirement/dirty takeover waits for all mprotects to finish.
    if (mprotect(g_watch_page.guest, g_watch_page.size, PROT_READ | PROT_WRITE)) {
        atomic_store(&g_watch_disarmed, 3); return 0;
    }
    expected = 1; atomic_compare_exchange_strong(&g_watch_disarmed, &expected, 2);
    if (first && atomic_load(&g_watch_disarmed) == 2) {
        watch_record r = {.reason = reason, .instruction = instruction, .pc = uc ? UC_PC(uc) : 0, .lr = uc ? UC_LR(uc) : 0};
        watch_enqueue(&r, uc ? UC_FP(uc) : 0);
    }
    return atomic_load(&g_watch_disarmed) == 2;
}
static int watch_fault(void *address, int is_write, void *context) {
    ucontext_t *uc = context; uint64_t esr = uc_esr(uc);
    // A first write to a fresh read-only VMA can carry a translation FSC: its
    // PTE is absent, but Linux rejects VM_WRITE before populating it. A lease
    // proves this managed allocation is live. Accept translation/access/permission
    // faults, never alignment, external/tag or instruction faults.
    unsigned fsc = esr & 63;
    if (!is_write || ((esr >> 26) & 63) != 0x24 || fsc < 4 || fsc > 15 || !dm_watch_page_acquire(&g_watch_page)) return 0;
    int saved_errno = errno, handled = 0;
    uintptr_t a = (uintptr_t)address, base = (uintptr_t)g_watch_page.guest;
    if (a >= base && a - base < g_watch_page.size) {
        uint32_t instruction = 0;
        if (atomic_load(&g_watch_disarmed)) handled = watch_disarm(WATCH_INSTRUCTION, 0, uc);
        else {
            watch_record r = {.pc = UC_PC(uc), .lr = UC_LR(uc), .old_value = watch_value()};
            uintptr_t fp = UC_FP(uc); // capture before base-register writeback can change FP/LR
            dm_watch_store_result result;
            if (dm_host_read(UC_PC(uc), &instruction, sizeof instruction) &&
                dm_watch_store(&g_watch_page, uc, instruction, g_watch_dczid, &result)) {
                handled = 1;
                if (watch_overlap(result.address, result.size, g_watch_at, g_watch_len)) {
                    r.address = result.address; r.size = result.size; r.new_value = watch_value();
                    watch_enqueue(&r, fp);
                }
            } else handled = watch_disarm(WATCH_INSTRUCTION, instruction, uc);
        }
    }
    dm_watch_page_release(&g_watch_page); errno = saved_errno; return handled;
}
static void watch_host_write(void *address, size_t size) {
    if (!dm_watch_page_acquire(&g_watch_page)) return;
    if (watch_overlap((uintptr_t)address, size, (uintptr_t)g_watch_page.guest, g_watch_page.size) &&
        !watch_disarm(WATCH_HOST_WRITE, 0, NULL)) die("WATCH cannot prepare host write: errno=%d", errno);
    dm_watch_page_release(&g_watch_page);
}
static void watch_prepare(uintptr_t base) {
    int rc = dm_watch_page_prepare(&g_watch_page, (void *)base, g_watch_pg);
    if (rc) die("WATCH fresh backing failed (%d, errno=%d)", rc, errno);
    atomic_store(&g_watch_disarmed, 0);
}
static void watch_arm(void) {
    if (g_watch_installed && !g_watch_foreign) {
        if (dm_watch_page_open(&g_watch_page)) die("WATCH protect failed: errno=%d", errno);
        LOG("WATCH %#llx armed generation=%llu (managed Android page)", (unsigned long long)g_watch_at, (unsigned long long)g_watch_page.generation);
    } else if (mprotect(g_watch_page.guest, g_watch_page.size, PROT_READ | PROT_WRITE)) die("WATCH open failed: errno=%d", errno);
}
void dm_android_watch_heap_reserved(void *heap, size_t size) {
    pthread_once(&g_watch_once, watch_configure); if (!g_watch_requested) return;
    pthread_mutex_lock(&g_watch_owner);
    uintptr_t base = (uintptr_t)heap;
    if (g_watch_relative) {
        if (g_watch_offset > size || g_watch_len > size - g_watch_offset) die("DM_PROBE_WATCH: offset exceeds guest heap");
        g_watch_at = base + g_watch_offset;
    }
    if (g_watch_at >= base && g_watch_at - base < size) g_watch_kind = 1;
    pthread_mutex_unlock(&g_watch_owner);
}
int dm_android_watch_open(void *address, size_t size) {
    pthread_once(&g_watch_once, watch_configure);
    if (!g_watch_requested) return mprotect(address, size, PROT_READ | PROT_WRITE);
    pthread_mutex_lock(&g_watch_owner);
    uintptr_t lo = (uintptr_t)address, target = g_watch_at & ~(g_watch_pg - 1);
    int rc = 0;
    if (g_watch_kind != 1 || target < lo || size < g_watch_pg || target - lo > size - g_watch_pg)
        rc = mprotect(address, size, PROT_READ | PROT_WRITE);
    else {
        if (!g_watch_page.alias) watch_prepare(target);
        if (target > lo) rc = mprotect(address, target - lo, PROT_READ | PROT_WRITE);
        uintptr_t end = lo + size, after = target + g_watch_pg;
        if (!rc && end > after) rc = mprotect((void *)after, end - after, PROT_READ | PROT_WRITE);
        if (!rc) watch_arm();
    }
    pthread_mutex_unlock(&g_watch_owner); return rc;
}
void dm_android_watch_release(uintptr_t lo, uintptr_t hi) {
    if (!g_watch_requested) return;
    pthread_mutex_lock(&g_watch_owner);
    if (g_watch_page.alias && (uintptr_t)g_watch_page.guest >= lo &&
        (uintptr_t)g_watch_page.guest < hi && dm_watch_page_retire(&g_watch_page))
        die("WATCH retire failed: errno=%d", errno);
    pthread_mutex_unlock(&g_watch_owner);
}
void dm_android_watch_module(module_t *m) {
    pthread_once(&g_watch_once, watch_configure); if (!g_watch_requested || g_watch_relative) return;
    uintptr_t base = (uintptr_t)m->base, target = g_watch_at & ~(g_watch_pg - 1);
    if (g_watch_at < base || g_watch_at - base >= m->size) return;
    uint64_t lo = dm_text_seal_end(m, g_watch_pg), hi = m->size;
    if (m->patch_off > 0 && ((uint64_t)m->patch_off & ~UINT64_C(0x3fff)) < hi) hi = (uint64_t)m->patch_off & ~UINT64_C(0x3fff);
    if (m->back_len && m->back_off < hi) hi = m->back_off;
    if (m->key_page_size && m->key_page_off < hi) hi = m->key_page_off;
    if (g_watch_at - base < m->data_off || target < base + lo || hi < g_watch_pg || target - base > hi - g_watch_pg)
        die("DM_PROBE_WATCH: target must be writable module data, outside text/patch/key pages");
    pthread_mutex_lock(&g_watch_owner); g_watch_kind = 2;
    // The loader has not copied or published this fresh allocation yet.
    if (mprotect((void *)target, g_watch_pg, PROT_NONE)) die("WATCH fresh module reservation failed");
    watch_prepare(target);
    if (mprotect((void *)target, g_watch_pg, PROT_READ | PROT_WRITE)) die("WATCH module initialization failed");
    pthread_mutex_unlock(&g_watch_owner);
}
void dm_android_watch_dirty_range(const void *address, size_t size,
                                 void (*acknowledge)(void *, uintptr_t), void *owner) {
    if (!g_watch_requested) return;
    pthread_mutex_lock(&g_watch_owner);
    uintptr_t target = g_watch_at & ~(g_watch_pg - 1);
    if (!g_watch_foreign && g_watch_at && watch_overlap((uintptr_t)address, size, target, g_watch_pg)) {
        g_watch_foreign = 1;
        // Called with the dirty range already published and before protection,
        // under its owner mutex. Draining prevents alias writes hiding a dirty
        // fault after takeover. Keep this exclusion across recommits: the pool
        // can retain its descriptor/backing beyond a guest allocation's life.
        if (atomic_load(&g_watch_page.gate) & DM_WATCH_PAGE_OPEN) acknowledge(owner, target);
        dm_watch_page_quiesce(&g_watch_page);
        if (g_watch_page.alias && !atomic_load(&g_watch_disarmed) &&
            mprotect(g_watch_page.guest, g_watch_page.size, PROT_READ | PROT_WRITE)) die("WATCH yield failed");
        LOG("WATCH disabled: renderer dirty tracker owns this page (restart to watch another page)");
    }
    pthread_mutex_unlock(&g_watch_owner);
}
void dm_android_watch_install(void) {
    pthread_once(&g_watch_once, watch_configure); if (!g_watch_requested) return;
    pthread_mutex_lock(&g_watch_owner);
    if (g_watch_installed) { pthread_mutex_unlock(&g_watch_owner); return; }
    g_watch_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_watch_wake < 0) die("WATCH eventfd failed");
    pthread_t thread; int rc = pthread_create(&thread, NULL, watch_logger, NULL);
    if (rc) die("WATCH logger: %s", strerror(rc));
    pthread_detach(thread);
    dm_fault_hook_register(watch_fault); dm_guest_write_prep_register(watch_host_write);
    g_watch_installed = 1;
    if (g_watch_page.alias) watch_arm();
    else LOG("WATCH waiting for managed guest heap/module data; arbitrary host allocations are unsupported");
    pthread_mutex_unlock(&g_watch_owner);
}
