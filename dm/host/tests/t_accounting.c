// Exercise the real collector against controlled proc records and this process.
#include "../memstat_android.c"
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

module_t *g_mod;
int g_nmod;
static dm_proc_range guest_bounds;
static const char *reuse_stat;
static void stat_file(const char *path, unsigned id, unsigned ticks, unsigned start);
void svc_guest_range(uint8_t **base, size_t *size) { *base = (uint8_t *)(uintptr_t)guest_bounds.lo; *size = guest_bounds.hi - guest_bounds.lo; }
int thr_name_of_tid(int tid, char *out, size_t n) {
    if (tid != 42) return 0;
    if (reuse_stat) stat_file(reuse_stat, 42, 5000, 790);
    snprintf(out, n, "0x1001 guest\tworker"); return 1;
}
static void put(const char *path, const char *text) { FILE *f = fopen(path, "w"); assert(f && fputs(text, f) >= 0 && !fclose(f)); }
static void stat_file(const char *path, unsigned id, unsigned ticks, unsigned start) {
    char text[512]; snprintf(text, sizeof text, "%u (worker ) (name)) R 1 2 3 4 5 6 7 8 9 10 %u 0 0 0 0 0 1 0 %u 0\n", id, ticks, start); put(path, text);
}
static void contents(FILE *f, char *text, size_t cap) {
    assert(!fflush(f) && !fseek(f, 0, SEEK_SET)); size_t n = fread(text, 1, cap - 1, f); text[n] = 0; assert(!fseek(f, 0, SEEK_END));
}
int main(void) {
    alarm(20); char dir[512], path[600], task[600], thread[600], threadstat[640], procstat[600];
    snprintf(dir, sizeof dir, "%s/accounting-XXXXXX", getenv("TMPDIR") ?: "/data/local/tmp"); assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/smaps", dir);
    const char *valid = "1000-3000 rw-p 0 00:00 0\nRss: 4 kB\nPss: 2 kB\nPrivate_Dirty: 1 kB\nSwap: 0 kB\n";
    put(path, valid); guest_bounds = (dm_proc_range){0x1000, 0x3000};
    FILE *out = tmpfile(), *regions = tmpfile(); assert(out && regions);
    char staging[600]; snprintf(staging, sizeof staging, "%s/regions-XXXXXX", dir);
    assert(!fchmod(fileno(out), 0640)); FILE *atomic_region = account_region_file(staging, out); assert(atomic_region);
    struct stat mode; assert(!fstat(fileno(atomic_region), &mode) && (mode.st_mode & 0777) == 0640);
    assert(!fclose(atomic_region) && !unlink(staging));
    assert(account_memory_sample(out, 2.0, path, regions));
    char text[8192]; contents(out, text, sizeof text); assert(strstr(text, "2.000\t8\t4\t2\t1\t0\t8\t4\t2\t1\t0\t") == text);
    contents(regions, text, sizeof text); assert(strstr(text, "0x1000\t0x3000\trw-p\tguest\t8\t4\t2\t1\t0"));
    long before = ftell(out); assert(before > 0);
    put(path, "1000-3000 rw-p 0 00:00 0\nRss: 4 kB\n"); assert(!account_memory_sample(out, 3.0, path, NULL) && ftell(out) == before);
    put(path, valid); assert(!chmod(path, 0)); assert(!account_memory_sample(out, 3.0, path, NULL) && ftell(out) == before); assert(!chmod(path, 0600));
    assert(!unlink(path)); assert(!account_memory_sample(out, 3.0, path, NULL) && ftell(out) == before);
    assert(!fclose(out) && !fclose(regions));

    snprintf(task, sizeof task, "%s/task", dir); assert(!mkdir(task, 0700));
    snprintf(thread, sizeof thread, "%s/task/42", dir); assert(!mkdir(thread, 0700));
    snprintf(threadstat, sizeof threadstat, "%s/stat", thread); snprintf(procstat, sizeof procstat, "%s/stat", dir);
    put(threadstat, "42 (worker\nname) R 1 2 3 4 5 6 7 8 9 10 10 0 0 0 0 0 1 0 789 0\n");
    dm_proc_cpu named; assert(account_read_cpu(threadstat, &named) && !strcmp(named.name, "worker name"));
    stat_file(threadstat, 42, 10, 789); stat_file(procstat, 9, 100, 123);
    account_cpu_state previous = {0}; out = tmpfile(); assert(out); uint64_t start = account_clock();
    assert(account_cpu_sample(out, &previous, start, 100, dir) && previous.count == 1 && ftell(out) == 0);
    previous.when -= 1000000000; stat_file(threadstat, 42, 30, 789); stat_file(procstat, 9, 150, 123);
    assert(account_cpu_sample(out, &previous, start, 100, dir));
    contents(out, text, sizeof text); assert(strstr(text, "\t42\t0x1001 guest worker\t") && strstr(text, "\t-\ttotal\t"));
    before = ftell(out); stat_file(threadstat, 42, 60, 789); previous.when -= 1000000000;
    reuse_stat = threadstat; assert(account_cpu_sample(out, &previous, start, 100, dir)); reuse_stat = NULL;
    assert(!fseek(out, before, SEEK_SET)); size_t renamed = fread(text, 1, sizeof text - 1, out); text[renamed] = 0;
    assert(strstr(text, "\t42\tworker ) (name)\t") && !strstr(text, "0x1001 guest")); assert(!fseek(out, 0, SEEK_END));
    before = ftell(out); stat_file(threadstat, 42, 5000, 790); previous.when -= 1000000000;
    assert(account_cpu_sample(out, &previous, start, 100, dir));
    assert(!fseek(out, before, SEEK_SET)); size_t got = fread(text, 1, sizeof text - 1, out); text[got] = 0;
    assert(!strstr(text, "\t42\t") && strstr(text, "\t-\ttotal\t")); assert(!fseek(out, 0, SEEK_END));
    assert(!unlink(threadstat) && !rmdir(thread));
    assert(account_cpu_sample(out, &previous, start, 100, dir) && previous.count == 0); // No stale TID entries.
    before = ftell(out); put(procstat, "broken\n"); assert(!account_cpu_sample(out, &previous, start, 100, dir) && ftell(out) == before);
    free(previous.thread); assert(!fclose(out) && !unlink(procstat) && !rmdir(task) && !rmdir(dir));

    size_t pg = (size_t)getpagesize(); uint8_t *memory = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(memory != MAP_FAILED);
    memset(memory, 0x5a, 4 * pg); guest_bounds = (dm_proc_range){(uintptr_t)memory, (uintptr_t)memory + 4 * pg};
    out = tmpfile(); assert(out && account_memory_sample(out, 0, "/proc/self/smaps", NULL));
    previous = (account_cpu_state){0}; long hz = sysconf(_SC_CLK_TCK); start = account_clock();
    assert(account_cpu_sample(out, &previous, start, hz, "/proc/self") && previous.count);
    uint64_t ticks = previous.process.ticks, until = account_clock() + 150000000;
    while (account_clock() < until) __asm__ volatile("" ::: "memory");
    assert(account_cpu_sample(out, &previous, start, hz, "/proc/self") && previous.process.ticks > ticks);
    free(previous.thread); assert(!fclose(out) && !munmap(memory, 4 * pg));
    alarm(0); puts("PASS: complete memory rows/regions, unavailable inputs, CPU lifecycle/reuse, named threads and actual own-process counters");
    return 0;
}
