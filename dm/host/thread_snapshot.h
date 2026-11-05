// Android diagnostics share the scheduler's SIGUSR2 handler. Peers are sampled
// asynchronously, not suspended together: no handler takes a host lock, logs,
// allocates, or waits for the inspecting thread.
#pragma once
#include "dm_uc.h"
#include "host_memory.h"
#include <dirent.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <time.h>

enum { SNAP_MAX = 256, SNAP_FRAMES = 10, SNAP_REQUEST = 1, SNAP_BUSY = 2, SNAP_DONE = 3 };
// Bind ownership to the TID in the same CAS as the state: a late handler must
// not claim a slot that a later inspection has assigned to another thread.
typedef struct {
    _Atomic uint64_t request;
    uint64_t frame[SNAP_FRAMES], sp, x[13], stack[128];
    uint32_t object[2][32];
    unsigned readable; // x19, x20, stack: complete reads only, never stale partial data.
} dm_snapshot;
static dm_snapshot snap_slots[SNAP_MAX];
static _Atomic int snap_ready, snap_active, snap_used, snap_inspecting;

static void snapshot_signal(ucontext_t *uc) {
    if (!atomic_load(&snap_active)) return;
    int tid = (int)syscall(SYS_gettid), count = atomic_load(&snap_used);
    for (int i = 0; i < count; i++) {
        dm_snapshot *s = &snap_slots[i];
        uint64_t target = (uint64_t)(unsigned)tid << 32, expected = target | SNAP_REQUEST;
        if (!atomic_compare_exchange_strong(&s->request, &expected, target | SNAP_BUSY)) continue;
        s->frame[0] = UC_PC(uc); s->frame[1] = UC_LR(uc); s->sp = UC_SP(uc);
        for (int k = 0; k < 3; k++) s->x[k] = UC_X(uc, k);
        for (int k = 19; k <= 28; k++) s->x[k - 16] = UC_X(uc, k);
        s->readable = 0;
        for (int k = 0; k < 2; k++)
            if (s->x[k + 3] >= 0x1000 && dm_host_read(s->x[k + 3], s->object[k], sizeof s->object[k])) s->readable |= 1u << k;
        if (s->sp >= 0x1000 && dm_host_read(s->sp, s->stack, sizeof s->stack)) s->readable |= 4;
        uint64_t fp = UC_FP(uc);
        for (int k = 2; k < SNAP_FRAMES; k++) {
            uint64_t pair[2]; s->frame[k] = 0;
            if (fp && !(fp & 7) && dm_host_read(fp, pair, sizeof pair)) {
                s->frame[k] = pair[1]; fp = pair[0] > fp ? pair[0] : 0;
            } else fp = 0;
        }
        // A timeout cannot recycle this slot while it is BUSY. Publishing DONE
        // is our last access; the next inspection may then reuse the storage.
        atomic_store(&s->request, target | SNAP_DONE);
        return;
    }
}
static uint64_t snapshot_clock(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
static void snapshot_where(const char *label, uint64_t address) {
    module_t *m = module_of((void *)(uintptr_t)address); Dl_info di;
    if (m) fprintf(stderr, " %s=%s+%#llx", label, m->name, (unsigned long long)(address - (uintptr_t)m->base));
    else if (dladdr((void *)(uintptr_t)address, &di) && di.dli_sname)
        fprintf(stderr, " %s=%s+%#llx", label, di.dli_sname, (unsigned long long)(address - (uintptr_t)di.dli_saddr));
    else fprintf(stderr, " %s=%#llx", label, (unsigned long long)address);
}
static int snapshot_dump(const char *tag, int guest_only) {
    if (!atomic_load(&snap_ready)) { LOG("[RED] %s thread snapshot: signal handler is not installed", tag); return -1; }
    int expected = 0;
    if (!atomic_compare_exchange_strong(&snap_inspecting, &expected, 1)) return -1; // Reentrant/concurrent dump.
    // A descheduled handler can outlive the timeout. Never clear or reuse its
    // buffer until its final release store; it resumes without waiting on us.
    for (int i = 0; i < SNAP_MAX; i++) if ((uint32_t)atomic_load(&snap_slots[i].request) == SNAP_BUSY) {
        LOG("[RED] %s thread snapshot: an earlier sample is still pending", tag);
        atomic_store(&snap_inspecting, 0); return -1;
    }
    DIR *dir = opendir("/proc/self/task");
    if (!dir) { LOG("[RED] %s thread snapshot: cannot enumerate threads", tag); atomic_store(&snap_inspecting, 0); return -1; }
    int count = 0, truncated = 0, self = (int)syscall(SYS_gettid); struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end; long tid = strtol(entry->d_name, &end, 10);
        if (*end || tid <= 0 || tid == self) continue;
        if (count == SNAP_MAX) { truncated = 1; continue; }
        dm_snapshot *s = &snap_slots[count++];
        atomic_store(&s->request, (uint64_t)(unsigned)tid << 32 | SNAP_REQUEST);
    }
    closedir(dir); atomic_store(&snap_used, count); atomic_store(&snap_active, 1);
    pid_t process = getpid();
    for (int i = 0; i < count; i++) {
        uint64_t target = atomic_load(&snap_slots[i].request) & ~UINT64_C(0xffffffff), request = target | SNAP_REQUEST;
        if (syscall(SYS_tgkill, process, (int)(target >> 32), SIGUSR2))
            atomic_compare_exchange_strong(&snap_slots[i].request, &request, target);
    }
    uint64_t deadline = snapshot_clock() + 20000000; // One 20 ms budget for the entire process.
    for (;;) {
        int pending = 0;
        for (int i = 0; i < count; i++) { uint32_t state = (uint32_t)atomic_load(&snap_slots[i].request); pending |= state == SNAP_REQUEST || state == SNAP_BUSY; }
        if (!pending || snapshot_clock() >= deadline) break;
        struct timespec wait = {0, 100000}; nanosleep(&wait, NULL);
    }
    atomic_store(&snap_active, 0);
    int sampled = 0;
    for (int i = 0; i < count; i++) {
        dm_snapshot *s = &snap_slots[i]; uint64_t request = atomic_load(&s->request), target = request & ~UINT64_C(0xffffffff);
        uint64_t pending = target | SNAP_REQUEST;
        atomic_compare_exchange_strong(&s->request, &pending, target);
        if ((uint32_t)atomic_load(&s->request) != SNAP_DONE) {
            LOG("[RED] %s thread snapshot tid=%d unavailable (blocked signal, exited or timed out)", tag, (int)(target >> 32));
            continue;
        }
        int guest = !guest_only;
        for (int k = 0; k < SNAP_FRAMES && !guest; k++) guest = module_of((void *)(uintptr_t)s->frame[k]) != NULL;
        if (!guest) continue;
        sampled++;
        flockfile(stderr);
        fprintf(stderr, "[dm] %s thread snapshot tid=%d sp=%#llx:", tag, (int)(target >> 32), (unsigned long long)s->sp);
        snapshot_where("pc", s->frame[0]); snapshot_where("lr", s->frame[1]);
        if (module_of((void *)(uintptr_t)s->frame[0])) {
            for (int k = 0; k < 13; k++) fprintf(stderr, " x%d=%#llx", k < 3 ? k : k + 16, (unsigned long long)s->x[k]);
        }
        for (int k = 2; k < SNAP_FRAMES && s->frame[k]; k++) snapshot_where("<", s->frame[k]);
        fputc('\n', stderr);
        if (module_of((void *)(uintptr_t)s->frame[0])) {
            for (int k = 0; k < 2; k++) if (s->readable & (1u << k)) {
                fprintf(stderr, "[dm] %s tid=%d x%d bytes at %#llx:", tag, (int)(target >> 32), k + 19, (unsigned long long)s->x[k + 3]);
                for (int j = 0; j < 32; j++) fprintf(stderr, " %08x", s->object[k][j]);
                fputc('\n', stderr);
            }
            if (s->readable & 4) for (int j = 0; j < 128; j += 8) {
                fprintf(stderr, "[dm] %s tid=%d stack sp+%#x:", tag, (int)(target >> 32), j * 8);
                for (int k = j; k < j + 8; k++) fprintf(stderr, " %016llx", (unsigned long long)s->stack[k]);
                fputc('\n', stderr);
            }
        }
        funlockfile(stderr);
    }
    if (truncated) LOG("[RED] %s thread snapshot truncated at %d threads", tag, SNAP_MAX);
    atomic_store(&snap_inspecting, 0); return sampled;
}
