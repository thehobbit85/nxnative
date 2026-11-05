// Linux /proc formats. Metrics retain Linux meanings; none is Mach footprint.
// https://docs.kernel.org/filesystems/proc.html
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

enum { DM_PROC_GUEST, DM_PROC_CODE, DM_PROC_HEAP, DM_PROC_GPU, DM_PROC_STACK, DM_PROC_FILE, DM_PROC_OTHER, DM_PROC_MIXED, DM_PROC_CLASSES };
enum { DM_PROC_METRICS = 5 }; // virtual, RSS, PSS, private dirty, swap; all KiB.
typedef struct { uint64_t lo, hi; } dm_proc_range;
typedef struct { uint64_t lo, hi, kib[DM_PROC_METRICS]; char permissions[5], path[512]; unsigned seen; } dm_proc_region;
typedef struct { unsigned id; uint64_t start, ticks; char name[64]; } dm_proc_cpu;

static int dm_proc_classify(const dm_proc_region *r, dm_proc_range guest, const dm_proc_range *code, size_t count) {
    if (guest.hi > guest.lo && r->lo < guest.hi && r->hi > guest.lo)
        return r->lo >= guest.lo && r->hi <= guest.hi ? DM_PROC_GUEST : DM_PROC_MIXED;
    int overlap = 0;
    for (size_t i = 0; i < count; i++) if (code[i].hi > code[i].lo && r->lo < code[i].hi && r->hi > code[i].lo) {
        if (r->lo >= code[i].lo && r->hi <= code[i].hi) return DM_PROC_CODE;
        overlap = 1;
    }
    if (overlap) return DM_PROC_MIXED;
    if (!strcmp(r->path, "[heap]") || !strncmp(r->path, "[anon:libc_malloc", 17) || !strncmp(r->path, "[anon:scudo:", 12)) return DM_PROC_HEAP;
    if (!strncmp(r->path, "[stack", 6) || !strncmp(r->path, "[anon:stack", 11)) return DM_PROC_STACK;
    if (!strncmp(r->path, "/dev/kgsl", 9) || !strncmp(r->path, "/dev/dri/", 9) || !strncmp(r->path, "/dev/mali", 9)) return DM_PROC_GPU;
    return r->path[0] == '/' ? DM_PROC_FILE : DM_PROC_OTHER;
}
static int dm_proc_uint(const char *text, const char **end, uint64_t *value) {
    if (*text < '0' || *text > '9') return 0;
    char *e; errno = 0; unsigned long long n = strtoull(text, &e, 10);
    if (errno || e == text) return 0;
    *end = e; *value = n; return 1;
}
static int dm_proc_smaps(FILE *f, int (*consume)(const dm_proc_region *, void *), void *arg) {
    char *line = NULL; size_t cap = 0; dm_proc_region r = {0}; int have = 0, ok = 1;
    while (getline(&line, &cap, f) >= 0) {
        unsigned long long lo, hi, offset, inode; unsigned major, minor; char perm[5]; int used = 0;
        if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n", &lo, &hi, perm, &offset, &major, &minor, &inode, &used) == 7 && used) {
            if (hi <= lo || (have && (r.seen != 15 || !consume(&r, arg)))) { ok = 0; break; }
            r = (dm_proc_region){.lo=lo, .hi=hi, .kib={(hi - lo) / 1024}}; have = 1;
            memcpy(r.permissions, perm, sizeof perm);
            char *path = line + used; path[strcspn(path, "\r\n")] = 0;
            snprintf(r.path, sizeof r.path, "%s", path);
            continue;
        }
        static const char *keys[] = {"Rss:", "Pss:", "Private_Dirty:", "Swap:"};
        for (int k = 0; k < 4; k++) if (!strncmp(line, keys[k], strlen(keys[k]))) {
            const char *p = line + strlen(keys[k]), *end; uint64_t n;
            while (*p == ' ' || *p == '\t') p++;
            if (!have || !dm_proc_uint(p, &end, &n)) { ok = 0; break; }
            while (*end == ' ' || *end == '\t') end++;
            if (strncmp(end, "kB", 2) || (end[2] && end[2] != '\n' && end[2] != '\r') || (r.seen & (1u << k))) { ok = 0; break; }
            r.kib[k + 1] = n; r.seen |= 1u << k;
        }
        if (!ok) break;
    }
    if (ferror(f) || !feof(f) || !have || r.seen != 15) ok = 0;
    if (ok && !consume(&r, arg)) ok = 0;
    free(line); return ok;
}
static int dm_proc_stat(const char *text, dm_proc_cpu *out) {
    const char *p; uint64_t id;
    if (!dm_proc_uint(text, &p, &id) || !id || id > INT_MAX || strncmp(p, " (", 2)) return 0;
    const char *name = p + 2, *close = strrchr(name, ')');
    if (!close || close[1] != ' ' || !close[2] || close[3] != ' ') return 0;
    dm_proc_cpu value = {.id=(unsigned)id}; uint64_t user = 0, system = 0;
    size_t bytes = (size_t)(close - name); if (bytes >= sizeof value.name) bytes = sizeof value.name - 1;
    memcpy(value.name, name, bytes);
    for (size_t i = 0; i < bytes; i++) if ((unsigned char)value.name[i] < 32) value.name[i] = ' ';
    p = close + 4;
    for (int field = 4; field <= 22; field++) {
        while (*p == ' ') p++;
        if (!*p || *p == '\n') return 0;
        const char *end = p; while (*end && *end != ' ' && *end != '\n') end++;
        if (field == 14 || field == 15 || field == 22) {
            uint64_t n; const char *used;
            if (!dm_proc_uint(p, &used, &n) || used != end) return 0;
            if (field == 14) user = n; else if (field == 15) system = n; else value.start = n;
        }
        p = end;
    }
    if (user > UINT64_MAX - system) return 0;
    value.ticks = user + system; *out = value; return 1;
}
static int dm_proc_percent(const dm_proc_cpu *old, const dm_proc_cpu *current, uint64_t elapsed_ns, long hz, double *out) {
    if (!elapsed_ns || hz <= 0 || old->id != current->id || old->start != current->start || current->ticks < old->ticks) return 0;
    *out = (double)(current->ticks - old->ticks) / (double)hz / ((double)elapsed_ns / 1e9) * 100;
    return 1;
}
