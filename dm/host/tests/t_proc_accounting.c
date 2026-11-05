// Kernel-format oracles for Android accounting; no live /proc needed here.
#include "../proc_accounting.h"
#include <assert.h>
#include <string.h>

static dm_proc_region rows[4];
static int count;
static int collect(const dm_proc_region *r, void *ctx) { (void)ctx; assert(count < 4); rows[count++] = *r; return 1; }
static int smaps(const char *text) {
    FILE *f = tmpfile(); assert(f && fputs(text, f) >= 0 && !fseek(f, 0, SEEK_SET)); count = 0;
    int ok = dm_proc_smaps(f, collect, NULL); assert(!fclose(f)); return ok;
}
int main(void) {
    const char *good = "1000-3000 rw-p 00000000 00:00 0 [anon:scudo:primary]\n"
        "Size: 8 kB\nRss: 4 kB\nPss: 2 kB\nPrivate_Dirty: 1 kB\nSwap: 0 kB\nVmFlags: rd wr\n"
        "4000-8000 r--s 00000000 00:01 4 /memfd:nxnative (deleted)\n"
        "Rss: 8 kB\nPss: 4 kB\nPrivate_Dirty: 0 kB\nSwap: 2 kB\n";
    assert(smaps(good) && count == 2);
    assert(rows[0].lo == 0x1000 && rows[0].hi == 0x3000 && rows[0].kib[0] == 8 && rows[0].kib[1] == 4 && rows[0].kib[2] == 2 && rows[0].kib[3] == 1);
    assert(rows[1].kib[0] == 16 && rows[1].kib[4] == 2 && !strcmp(rows[1].path, "/memfd:nxnative (deleted)"));
    dm_proc_range guest = {0x1000, 0x3000}, code[] = {{0x4000, 0x8000}};
    assert(dm_proc_classify(&rows[0], guest, code, 1) == DM_PROC_GUEST);
    assert(dm_proc_classify(&rows[1], guest, code, 1) == DM_PROC_CODE);
    guest.lo = 0x2000; assert(dm_proc_classify(&rows[0], guest, code, 1) == DM_PROC_MIXED);
    guest = (dm_proc_range){0}; assert(dm_proc_classify(&rows[0], guest, code, 1) == DM_PROC_HEAP);
    code[0].hi = 0x6000; assert(dm_proc_classify(&rows[1], guest, code, 1) == DM_PROC_MIXED);
    code[0].hi = code[0].lo; assert(dm_proc_classify(&rows[1], guest, code, 1) == DM_PROC_FILE);
    const char *names[] = {"[anon:stack_and_tls:19]", "/dev/kgsl-3d0", "/dev/dri/renderD128", "[anon:dalvik-main space]", ""};
    const int classes[] = {DM_PROC_STACK, DM_PROC_GPU, DM_PROC_GPU, DM_PROC_OTHER, DM_PROC_OTHER};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) { snprintf(rows[0].path, sizeof rows[0].path, "%s", names[i]); assert(dm_proc_classify(&rows[0], guest, NULL, 0) == classes[i]); }
    assert(!smaps(""));
    assert(!smaps("1000-3000 rw-p 0 00:00 0\nRss: 4 kB\n"));
    assert(!smaps("1000-3000 rw-p 0 00:00 0\nRss: bad kB\nPss: 0 kB\nPrivate_Dirty: 0 kB\nSwap: 0 kB\n"));
    assert(!smaps("1000-3000 rw-p 0 00:00 0\nRss: -1 kB\nPss: 0 kB\nPrivate_Dirty: 0 kB\nSwap: 0 kB\n"));
    dm_proc_cpu cpu;
    // Fields 14/15 are user/system ticks, field22 is starttime. The name can include ')'.
    assert(dm_proc_stat("42 (worker ) (name)) S 1 2 3 4 5 6 7 8 9 10 30 20 0 0 0 0 1 0 789 0\n", &cpu));
    assert(cpu.id == 42 && cpu.start == 789 && cpu.ticks == 50 && !strcmp(cpu.name, "worker ) (name)"));
    dm_proc_cpu old = cpu; old.ticks = 25; double pct;
    assert(dm_proc_percent(&old, &cpu, 500000000, 100, &pct) && pct == 50);
    cpu.start++; assert(!dm_proc_percent(&old, &cpu, 500000000, 100, &pct)); cpu.start--;
    cpu.id++; assert(!dm_proc_percent(&old, &cpu, 500000000, 100, &pct)); cpu.id--;
    cpu.ticks = 24; assert(!dm_proc_percent(&old, &cpu, 500000000, 100, &pct));
    assert(!dm_proc_percent(&old, &cpu, 0, 100, &pct));
    assert(!dm_proc_stat("42 (unterminated S 1 2 3", &cpu));
    assert(!dm_proc_stat("42 (name) S 1 2 3", &cpu));
    assert(!dm_proc_stat("42 (name) S 1 2 3 4 5 6 7 8 9 10 -1 20 0 0 0 0 1 0 789", &cpu));
    puts("PASS: smaps completeness/units, classification boundaries and stat identity/tick accounting");
    return 0;
}
