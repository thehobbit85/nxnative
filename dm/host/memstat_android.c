// Target-native equivalents of memstat.m. Own-process /proc only; no root,
// debugger or Mach/Metal accounting assumptions. Enabled explicitly after load.
#include "dm.h"
#include "proc_accounting.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

void svc_guest_range(uint8_t **base, size_t *size);
int thr_name_of_tid(int tid, char *out, size_t n);
static dm_proc_range *account_code;
static size_t account_code_count;
static const char *account_class[] = {"guest", "code", "heap", "gpu_mapping", "stack", "file", "other", "mixed"};
static const char *account_metric[] = {"virtual", "rss", "pss", "private_dirty", "swap"};
static uint64_t account_clock(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
typedef struct { uint64_t kib[DM_PROC_CLASSES][DM_PROC_METRICS]; dm_proc_range guest; FILE *regions; } account_memory;
static int account_region(const dm_proc_region *r, void *arg) {
    account_memory *m = arg;
    int c = dm_proc_classify(r, m->guest, account_code, account_code_count);
    for (int k = 0; k < DM_PROC_METRICS; k++) {
        if (m->kib[c][k] > UINT64_MAX - r->kib[k]) return 0;
        m->kib[c][k] += r->kib[k];
    }
    if (m->regions) {
        fprintf(m->regions, "%#llx\t%#llx\t%s\t%s", (unsigned long long)r->lo, (unsigned long long)r->hi, r->permissions, account_class[c]);
        for (int k = 0; k < DM_PROC_METRICS; k++) fprintf(m->regions, "\t%llu", (unsigned long long)r->kib[k]);
        fputc('\t', m->regions);
        for (const char *p = r->path; *p; p++) fputc((unsigned char)*p < 32 ? ' ' : *p, m->regions);
        fputc('\n', m->regions);
        if (ferror(m->regions)) return 0;
    }
    return 1;
}
static int account_memory_sample(FILE *output, double seconds, const char *smaps, FILE *regions) {
    FILE *f = fopen(smaps, "r"); if (!f) return 0;
    uint8_t *base; size_t size; svc_guest_range(&base, &size);
    account_memory m = {.guest={(uintptr_t)base, (uintptr_t)base + size}, .regions=regions};
    int ok = dm_proc_smaps(f, account_region, &m); if (fclose(f)) ok = 0;
    if (!ok) return 0;
    uint64_t total[DM_PROC_METRICS] = {0};
    for (int k = 0; k < DM_PROC_METRICS; k++) {
        for (int c = 0; c < DM_PROC_CLASSES; c++) { if (total[k] > UINT64_MAX - m.kib[c][k]) return 0; total[k] += m.kib[c][k]; }
    }
    fprintf(output, "%.3f", seconds);
    for (int k = 0; k < DM_PROC_METRICS; k++) fprintf(output, "\t%llu", (unsigned long long)total[k]);
    for (int c = 0; c < DM_PROC_CLASSES; c++) for (int k = 0; k < DM_PROC_METRICS; k++) fprintf(output, "\t%llu", (unsigned long long)m.kib[c][k]);
    fputc('\n', output); return !ferror(output) && !fflush(output);
}
static FILE *account_region_file(char *temp, FILE *report) {
    int fd = mkstemp(temp); if (fd < 0) return NULL;
    // External app storage uses the report's group access for diagnostic
    // retrieval. Preserve the selected report's mode across atomic publication.
    struct stat st; FILE *f = NULL;
    if (!fstat(fileno(report), &st) && !fchmod(fd, st.st_mode & 0777)) f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temp); }
    return f;
}
static void *account_memory_thread(void *arg) {
    char *path = arg; FILE *out = fopen(path, "w");
    if (!out) { LOG("[RED] DM_MEMSTAT: cannot open %s (errno=%d)", path, errno); free(path); return NULL; }
    fprintf(out, "# Linux KiB: virtual/RSS/PSS/private dirty/swap; RSS and swap may count aliases repeatedly. PSS is proportional, not Mach footprint. GPU mappings exclude driver-private allocations; unrecognized anonymous maps are other; boundary-spanning maps are mixed.\nseconds");
    for (int k = 0; k < DM_PROC_METRICS; k++) fprintf(out, "\ttotal.%s_kib", account_metric[k]);
    for (int c = 0; c < DM_PROC_CLASSES; c++) for (int k = 0; k < DM_PROC_METRICS; k++) fprintf(out, "\t%s.%s_kib", account_class[c], account_metric[k]);
    fputc('\n', out); fflush(out);
    const char *ds = getenv("DM_MEMSTAT_DUMP"); long dump_at = -1;
    if (ds) { char *end; errno = 0; long n = strtol(ds, &end, 10); if (!errno && end != ds && !*end && n >= 0) dump_at = n; else LOG("[RED] DM_MEMSTAT_DUMP: expected nonnegative seconds"); }
    uint64_t start = account_clock(); int unavailable = 0, dumped = 0;
    for (;;) {
        double seconds = (account_clock() - start) / 1e9;
        FILE *regions = NULL; char *temp = NULL, *dest = NULL;
        if (!dumped && dump_at >= 0 && seconds >= dump_at) {
            dumped = 1;
            if (asprintf(&dest, "%s.regions", path) >= 0 && asprintf(&temp, "%s.tmpXXXXXX", dest) >= 0) {
                regions = account_region_file(temp, out);
                if (regions) fputs("start\tend\tpermissions\tclass\tvirtual_kib\trss_kib\tpss_kib\tprivate_dirty_kib\tswap_kib\tpath\n", regions);
            }
            if (!regions) LOG("[RED] DM_MEMSTAT_DUMP: cannot create region output");
        }
        int ok = account_memory_sample(out, seconds, "/proc/self/smaps", regions);
        if (regions) { if (fclose(regions)) ok = 0; if (ok && rename(temp, dest)) { ok = 0; LOG("[RED] DM_MEMSTAT_DUMP: publish failed (errno=%d)", errno); } }
        if (temp) unlink(temp);
        free(temp); free(dest);
        if (!ok && !unavailable) { LOG("[RED] DM_MEMSTAT: sample unavailable (proc access, incomplete data or output error)"); fputs("# unavailable\n", out); fflush(out); }
        unavailable = !ok;
        if (ferror(out)) break;
        sleep(1);
    }
    fclose(out); free(path); return NULL;
}

typedef struct { dm_proc_cpu *thread; size_t count; dm_proc_cpu process; uint64_t when; } account_cpu_state;
static int account_read_cpu(const char *path, dm_proc_cpu *out) {
    errno = 0; FILE *f = fopen(path, "r"); if (!f) return errno == ENOENT || errno == ESRCH ? -1 : 0;
    // comm may contain newlines as well as spaces/parentheses. Read the bounded
    // complete record, not its first line, before finding the final ')'.
    char record[4096]; size_t size = fread(record, 1, sizeof record - 1, f); record[size] = 0;
    int ok = size && (size < sizeof record - 1 || fgetc(f) == EOF) && dm_proc_stat(record, out);
    if (ferror(f)) ok = 0;
    if (fclose(f)) ok = 0;
    return ok;
}
static int account_cpu_sample(FILE *out, account_cpu_state *previous, uint64_t start, long hz, const char *root) {
    uint64_t now = account_clock(); char path[512];
    snprintf(path, sizeof path, "%s/stat", root); dm_proc_cpu process;
    if (account_read_cpu(path, &process) != 1) return 0;
    snprintf(path, sizeof path, "%s/task", root); DIR *dir = opendir(path); if (!dir) return 0;
    dm_proc_cpu *current = NULL; size_t count = 0, capacity = 0; int ok = 1; struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) ok = 0; break; }
        const char *end; uint64_t tid;
        if (!dm_proc_uint(entry->d_name, &end, &tid) || *end || !tid || tid > INT_MAX) { errno = 0; continue; }
        snprintf(path, sizeof path, "%s/task/%u/stat", root, (unsigned)tid); dm_proc_cpu value; errno = 0;
        int read = account_read_cpu(path, &value);
        if (read != 1) {
            if (read != -1) { ok = 0; break; }
            errno = 0; continue; // A thread can exit between enumeration and open.
        }
        if (value.id != tid) { ok = 0; break; }
        if (count == capacity) {
            if (capacity == 4096) { ok = 0; break; }
            size_t next = capacity ? capacity * 2 : 64; void *p = realloc(current, next * sizeof *current);
            if (!p) { ok = 0; break; } current = p; capacity = next;
        }
        current[count++] = value; errno = 0;
    }
    closedir(dir);
    if (!ok) { free(current); return 0; }
    if (previous->when && now > previous->when) {
        uint64_t dt = now - previous->when; double pct;
        for (size_t i = 0; i < count; i++) for (size_t j = 0; j < previous->count; j++) if (current[i].id == previous->thread[j].id) {
            if (dm_proc_percent(&previous->thread[j], &current[i], dt, hz, &pct) && pct >= 0.5) {
                char guest[96]; const char *name = current[i].name;
                if (thr_name_of_tid((int)current[i].id, guest, sizeof guest)) {
                    // Do not label an old sample with a new guest that reused
                    // its TID during lookup. No scheduler lock spans proc IO.
                    dm_proc_cpu confirm;
                    snprintf(path, sizeof path, "%s/task/%u/stat", root, current[i].id);
                    if (account_read_cpu(path, &confirm) == 1 && confirm.id == current[i].id && confirm.start == current[i].start) name = guest;
                }
                fprintf(out, "%.3f\t%u\t", (now - start) / 1e9, current[i].id);
                for (const char *p = name; *p; p++) fputc((unsigned char)*p < 32 ? ' ' : *p, out);
                fprintf(out, "\t%.2f\n", pct);
            }
            break;
        }
        if (dm_proc_percent(&previous->process, &process, dt, hz, &pct)) fprintf(out, "%.3f\t-\ttotal\t%.2f\n", (now - start) / 1e9, pct);
    }
    free(previous->thread); *previous = (account_cpu_state){current, count, process, now};
    return !ferror(out) && !fflush(out);
}
static void *account_cpu_thread(void *arg) {
    char *spec = arg, *comma = strrchr(spec, ','); long interval = 10;
    if (comma) {
        char *end; errno = 0; interval = strtol(comma + 1, &end, 10);
        if (errno || end == comma + 1 || *end || interval <= 0 || interval > 86400) { LOG("[RED] DM_THRCPU: interval must be 1..86400 seconds"); free(spec); return NULL; }
        *comma = 0;
    }
    long hz = sysconf(_SC_CLK_TCK); FILE *out = hz > 0 ? fopen(spec, "w") : NULL;
    if (!out) { LOG("[RED] DM_THRCPU: output or clock unavailable (errno=%d)", errno); free(spec); return NULL; }
    fprintf(out, "# CPU%%: 100 = one core; task ticks / elapsed monotonic time, CLK_TCK=%ld. New/reused TIDs need two samples; total uses process counters including exited threads. Threads below 0.5%% are omitted.\nseconds\tthread ID\tthread name\tCPU%%\n", hz); fflush(out);
    account_cpu_state previous = {0}; uint64_t start = account_clock(); int unavailable = 0;
    for (;;) {
        int ok = account_cpu_sample(out, &previous, start, hz, "/proc/self");
        if (!ok && !unavailable) { LOG("[RED] DM_THRCPU: sample unavailable (proc access, incomplete data or output error)"); fputs("# unavailable\n", out); fflush(out); }
        unavailable = !ok; if (ferror(out)) break;
        sleep((unsigned)interval);
    }
    free(previous.thread); fclose(out); free(spec); return NULL;
}
static void account_start_once(void) {
    const char *mem = getenv("DM_MEMSTAT"), *cpu = getenv("DM_THRCPU");
    if ((!mem || !*mem) && (!cpu || !*cpu)) return;
    account_code = calloc((size_t)g_nmod, sizeof *account_code);
    if (g_nmod && !account_code) { LOG("[RED] diagnostics: module accounting allocation failed"); return; }
    for (int i = 0; i < g_nmod; i++) {
        uintptr_t base = (uintptr_t)g_mod[i].base;
        account_code[account_code_count++] = (dm_proc_range){base + (g_mod[i].patch_off < 0 ? g_mod[i].patch_off : 0), base + g_mod[i].size};
    }
    const char *spec[] = {mem, cpu}; void *(*worker[])(void *) = {account_memory_thread, account_cpu_thread};
    for (int i = 0; i < 2; i++) if (spec[i] && *spec[i]) {
        char *copy = strdup(spec[i]); pthread_t thread;
        int error = copy ? pthread_create(&thread, NULL, worker[i], copy) : ENOMEM;
        if (error) { free(copy); LOG("[RED] diagnostics: accounting worker unavailable (%d)", error); }
        else pthread_detach(thread);
    }
}
void dm_android_accounting_start(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, account_start_once);
}
