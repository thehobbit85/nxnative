// Instruction breakpoints installed during preload, before the guest can execute text.
#include "dm.h"
#include "dm_uc.h"
#include "host_memory.h"
#include <sys/mman.h>
#include <dlfcn.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if !TARGET_OS_IPHONE
void crash_set_trap_hook(int (*fn)(ucontext_t *));
// Syntax module:offset[@x0 offset]. Put a brk at the guest address; on hit print x0..x3 (and the readable strings they point to) and a backtrace, then execute the original instruction from a trampoline slot and jump back.
// The trampoline borrows x16: place probes only at function entries or at points where x16 is dead; the original instruction must not be PC-relative (adr/adrp/b/bl/ldr literal/cbz...).
#define MAX_PROBE 16
typedef void (*probe_fn)(ucontext_t *uc);
static struct { uint64_t at; uint32_t *slot; unsigned hits, seen; char name[40]; long dump, dpost; probe_fn fn; int freg; long foff; uint64_t fval; int hreg; } g_probe[MAX_PROBE];   // dpost: @*xN+off+dpost

// !xN: treat xN as a sead::ExpHeap and print its free and used blocks (block size at block +0x18, alignment pad at +0x12). Settings (docs/agents/05-debugging.md):
// DM_PROBE_HEAP=<name>,<start>,<size>,<free list>,<used list> layout by sead version (hex; node offset at list +0x14), default SMBW 0,28,30,b0,c8 (no name), BotW 28,38,40,e0,f8;
// DM_PROBE_HEAPNAME=<text> only heaps whose name contains it; DM_PROBE_HEAPWORDS=1 each used block's first two words; DM_PROBE_HEAPFRAME=<vtable offset in main> heaps of that class
// are frame heaps (head +0xe0, tail +0xe8); DM_PROBE_HEAPRAW=1 the heap object's first 0x130 bytes
static struct { int init, words, raw; long lay[5], frame_vt; const char *want; } g_hw = { .lay = { 0, 0x28, 0x30, 0xb0, 0xc8 } };
static void probe_heapcfg(void) {
    if (g_hw.init) return; g_hw.init = 1;
    const char *e = getenv("DM_PROBE_HEAP"); if (e) sscanf(e, "%lx,%lx,%lx,%lx,%lx", &g_hw.lay[0], &g_hw.lay[1], &g_hw.lay[2], &g_hw.lay[3], &g_hw.lay[4]);
    e = getenv("DM_PROBE_HEAPFRAME"); g_hw.frame_vt = e ? strtol(e, 0, 16) : -1;
    g_hw.want = getenv("DM_PROBE_HEAPNAME"); g_hw.words = getenv("DM_PROBE_HEAPWORDS") != NULL; g_hw.raw = getenv("DM_PROBE_HEAPRAW") != NULL;
}
static int rd64(uint64_t a, uint64_t *v) { return dm_host_read(a, v, sizeof *v); }
static void probe_word(uint64_t v) {   // A word as module+offset when it points into a module (a vtable names the object)
    module_t *m = v > 0x10000 ? module_of((void *)v) : NULL;
    if (m) fprintf(stderr, "%s+%llx", m->name, (unsigned long long)(v - (uint64_t)m->base)); else fprintf(stderr, "%llx", (unsigned long long)v);
}
static void probe_heapname(uint64_t h, char *nm, size_t n) {   // sead heaps are named (resource heaps after their file)
    uint64_t np = 0; nm[0] = 0;
    if (g_hw.lay[0] && rd64(h + g_hw.lay[0], &np) && np) { size_t g = dm_host_read_some(np, nm, n - 1); nm[g < n ? g : n - 1] = 0; for (int k = 0; nm[k]; k++) if (nm[k] < 0x20 || nm[k] > 0x7e) { nm[k] = 0; break; } }
}
static void probe_heapwalk(uint64_t h) {
    char nm[128]; probe_heapname(h, nm, sizeof nm);
    uint64_t start = 0, size = 0, vt = 0; rd64(h + g_hw.lay[1], &start); rd64(h + g_hw.lay[2], &size); rd64(h, &vt);
    module_t *vm = vt ? module_of((void *)vt) : NULL;
    if (g_hw.frame_vt > 0 && vm && !strcmp(vm->name, "main") && vt - (uint64_t)vm->base == (uint64_t)g_hw.frame_vt) {
        uint64_t hd = 0, tl = 0; rd64(h + 0xe0, &hd); rd64(h + 0xe8, &tl);
        fprintf(stderr, "[dm]   FrameHeap %#llx \"%s\" size %#llx head +%#llx tail +%#llx free %#llx\n", (unsigned long long)h, nm, (unsigned long long)size, (unsigned long long)(hd - start), (unsigned long long)(tl - start), (unsigned long long)(tl - hd));
        return;
    }
    fprintf(stderr, "[dm]   ExpHeap %#llx \"%s\" start %#llx size %#llx\n", (unsigned long long)h, nm, (unsigned long long)start, (unsigned long long)size);
    if (g_hw.raw) { fprintf(stderr, "[dm]   heap words:"); for (int k = 0; k < 0x130; k += 8) { uint64_t v = 0; rd64(h + k, &v); fprintf(stderr, "%s+%x=", k % 64 ? " " : "\n[dm]     ", k); probe_word(v); } fputc('\n', stderr); }
    static const char *L[2] = { "free", "used" };
    for (int k = 0; k < 2; k++) {
        uint64_t list = h + g_hw.lay[3 + k], offw = 0, node = 0, sum = 0, pads = 0; int n = 0; rd64(list + 0x14, &offw); int32_t off = (int32_t)offw;
        rd64(list + 8, &node);
        fprintf(stderr, "[dm]   %s:", L[k]);
        while (node && node != list && n < 4000) {
            uint64_t blk = node - off, bsz = 0, w2 = 0; rd64(blk + 0x18, &bsz); rd64(blk + 0x10, &w2);
            unsigned pad = (unsigned)(w2 >> 16) & 0xffff;
            if (g_hw.words && k) {
                uint64_t a = 0, b = 0; rd64(blk + 0x20, &a); rd64(blk + 0x28, &b);
                fprintf(stderr, "\n[dm]     %llx+%llx pad %x: ", (unsigned long long)(blk - start), (unsigned long long)bsz, pad); probe_word(a); fputc(' ', stderr); probe_word(b);
            } else {
                fprintf(stderr, "%s%llx+%llx", n % 8 ? " " : "\n[dm]     ", (unsigned long long)(blk - start), (unsigned long long)bsz);
                if (pad) fprintf(stderr, "p%x", pad);
            }
            sum += bsz; pads += pad; n++;
            if (!rd64(node + 8, &node)) break;
        }
        fprintf(stderr, "\n[dm]   %s %d blocks %#llx bytes, alignment pads %#llx\n", L[k], n, (unsigned long long)sum, (unsigned long long)pads);
    }
}   // dump: on hit print 16 bytes at [x0+dump] (-1 = do not print); fn: dedicated handler (replaces the default print)
static int g_nprobe;
static uint64_t g_probe_frame_first, g_probe_frame_last = UINT64_MAX;
static unsigned g_probe_limit, g_probe_every;   // DM_PROBE_LIMIT printed hits per probe; DM_PROBE_EVERY=N every hit counts, every N-th prints (#N is then the true hit count: rates)
uint64_t dm_guest_frame(void);
static void probe_str(uint64_t p) {
    char buf[80]; size_t got = 0;
    if (p < 0x1000 || !(got = dm_host_read_some(p, buf, sizeof buf - 1))) return;
    buf[got] = 0; int n = 0; while (buf[n] >= 0x20 && buf[n] < 0x7f) n++;
    if (n >= 3 && (buf[n] == 0 || n == (int)got)) { fprintf(stderr, " \"%.*s\"", n, buf); return; }
    // Game string object {char* pointer, u64 length, u64 capacity|top bit} (the path argument of 0xdf67f0 FJSONUtils has exactly this layout)
    if (got >= 24) {
        uint64_t ptr, len; memcpy(&ptr, buf, 8); memcpy(&len, buf + 8, 8);
        char t[96];
        if (len >= 3 && len < sizeof t && dm_host_read(ptr, t, len)) {
            int k = 0; while (k < (int)len && t[k] >= 0x20 && t[k] < 0x7f) k++;
            if (k == (int)len) fprintf(stderr, " S\"%.*s\"", (int)len, t);
        }
    }
}
// DM_PROBE_DUMP=module:offset:length[:file]: on the first hit of any probe, write that range of guest memory to a file (default run/platform/probe_dump.bin)
static void probe_dump_once(void) {
    static int done; const char *e = getenv("DM_PROBE_DUMP"); if (done || !e) return; done = 1;
    char spec[512]; strlcpy(spec, e, sizeof spec);
    char *mod = strtok(spec, ":"), *o = strtok(NULL, ":"), *n = strtok(NULL, ":"), *f = strtok(NULL, ":");
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (mod && !strcmp(g_mod[i].name, mod)) m = &g_mod[i];
    if (!m || !o || !n) { LOG("DM_PROBE_DUMP format: module:offset:length[:file]"); return; }
    char def[512]; snprintf(def, sizeof def, "%s%s/run/platform/probe_dump.bin", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work");
    FILE *fp = fopen(f ? f : def, "wb"); if (!fp) return;
    fwrite(m->base + strtoull(o, 0, 16), 1, strtoull(n, 0, 16), fp); fclose(fp);
    LOG("probe dump %s+%s length %s → %s", mod, o, n, f ? f : def);
}
static uint64_t probe_reg(ucontext_t *uc, int reg) {
    if (reg == 29) return UC_FP(uc);
    if (reg == 30) return UC_LR(uc);
    if (reg == 31) return UC_SP(uc);
    return UC_X(uc, reg);
}

static int probe_hit(ucontext_t *uc) {   // Registered by probe_install with the SIGTRAP branch of crash.c
    uint64_t pc = UC_PC(uc);
    for (int i = 0; i < g_nprobe; i++) if (g_probe[i].at == pc) {
        if (g_probe[i].fn) { g_probe[i].hits++; g_probe[i].fn(uc); UC_PC(uc) = (uint64_t)g_probe[i].slot; return 1; }
        uint64_t frame = dm_guest_frame();
        if (g_probe_every && frame >= g_probe_frame_first && frame <= g_probe_frame_last && ++g_probe[i].seen % g_probe_every) { UC_PC(uc) = (uint64_t)g_probe[i].slot; return 1; }
        if (frame < g_probe_frame_first || frame > g_probe_frame_last || (g_probe_limit && g_probe[i].hits >= g_probe_limit)) {
            UC_PC(uc) = (uint64_t)g_probe[i].slot; return 1;
        }
        if (g_probe[i].freg >= 0) {   // ?xN=value / ?[xN+offset]=value: pass through silently when it does not match
            uint64_t v = probe_reg(uc, g_probe[i].freg);
            if (g_probe[i].foff >= 0 && !rd64(v + g_probe[i].foff, &v)) v = ~g_probe[i].fval;
            if (v != g_probe[i].fval) { UC_PC(uc) = (uint64_t)g_probe[i].slot; return 1; }
        }
        if (g_probe[i].hreg >= 0 && g_hw.want) {   // !xN with DM_PROBE_HEAPNAME: report only hits on heaps of that name
            char nm[128]; probe_heapname(probe_reg(uc, g_probe[i].hreg), nm, sizeof nm);
            if (!strstr(nm, g_hw.want)) { UC_PC(uc) = (uint64_t)g_probe[i].slot; return 1; }
        }
        probe_dump_once();
        flockfile(stderr);   // One block per hit when threads hit at once
        { extern uint64_t g_t0_ns; uint64_t now_ns(void); uint32_t thr_cur_handle(void); ++g_probe[i].hits; fprintf(stderr, "[dm] PROBE %s #%u [%.4fs] F=%llu T=%x", g_probe[i].name, g_probe_every ? g_probe[i].seen : g_probe[i].hits, (now_ns() - g_t0_ns) / 1e9, (unsigned long long)frame, thr_cur_handle()); }
        for (int r = 0; r < 8; r++) { uint64_t v = probe_reg(uc, r); fprintf(stderr, " x%d=%#llx", r, (unsigned long long)v); probe_str(v); }
        for (int r = 19; r < 24; r++) fprintf(stderr, " x%d=%#llx", r, (unsigned long long)probe_reg(uc, r));
        if (getenv("DM_PROBE_ALL")) for (int r = 8; r < 19; r++) fprintf(stderr, " x%d=%#llx", r, (unsigned long long)probe_reg(uc, r));   // also print x8–x18
        fprintf(stderr, " lr=%#llx", (unsigned long long)UC_LR(uc));
        { uint64_t x8 = probe_reg(uc, 8); Dl_info di; fprintf(stderr, " x8=%#llx", (unsigned long long)x8);   // x8: indirect call targets are often here (probe placed before blr x8)
          if (dladdr((void *)x8, &di) && di.dli_sname) fprintf(stderr, " (%s+%#lx)", di.dli_sname, (unsigned long)(x8 - (uint64_t)di.dli_saddr)); }
        if (g_probe[i].dump >= 0) {
            uint8_t b[16];
            int dr = (g_probe[i].dump >> 25) & 31;                                    // @xN+offset: based on xN
            uint64_t at = probe_reg(uc, dr) + (g_probe[i].dump & 0xffffff);
            if (g_probe[i].dump & 0x1000000) { uint64_t p = 0; if (rd64(at, &p) && p) at = p + g_probe[i].dpost; else at = 0; }   // @*offset[+post]: dereference first, then dump (post bytes further on)
            if (at && dm_host_read(at, b, 16)) {
                fprintf(stderr, " [%sx%d+%#lx]", (g_probe[i].dump & 0x1000000) ? "*" : "", dr, g_probe[i].dump & 0xffffff);
                if (g_probe[i].dpost) fprintf(stderr, "+%#lx", g_probe[i].dpost);
                fputc('=', stderr); for (int k = 0; k < 16; k++) fprintf(stderr, "%02x", b[k]); }
        }
        fputc('\n', stderr);
        { static long xd = -2; if (xd == -2) { const char *e = getenv("DM_PROBE_X2"); xd = e ? strtol(e, 0, 16) : -1; }   // DM_PROBE_X2=bytes: also print the memory x2 points to (to read object fields)
          if (xd > 0) { uint64_t at = probe_reg(uc, 2); uint8_t b[512]; size_t got = 0; long n = xd > 512 ? 512 : xd;
            if ((got = dm_host_read_some(at, b, n))) { fprintf(stderr, "[dm]   [x2]:"); for (long k = 0; k < (long)got; k += 8) { uint64_t v = 0; size_t left = got - (size_t)k; memcpy(&v, b + k, left < sizeof v ? left : sizeof v); fprintf(stderr, "%s%llx", k % 32 ? " " : "\n[dm]   ", (unsigned long long)v); } fputc('\n', stderr); } } }
        if (g_probe[i].hreg >= 0) probe_heapwalk(probe_reg(uc, g_probe[i].hreg));
        dm_backtrace(UC_FP(uc));
        funlockfile(stderr);
        UC_PC(uc) = (uint64_t)g_probe[i].slot;
        return 1;
    }
    return 0;
}
static uint32_t *g_slots; static size_t g_slot_size; static int g_slots_sealed;
static void probe_add(module_t *m, uint64_t off, long dump, probe_fn fn) {
    if (g_nprobe >= MAX_PROBE) die("too many probes");
    if (!g_slots) {
        g_slot_size = (size_t)getpagesize();
        g_slots = mmap(NULL, g_slot_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_slots == MAP_FAILED) die("probe trampoline allocation failed");
    }
    uint64_t at = (uint64_t)m->base + off;
    uint32_t *w = (uint32_t *)at, *sl = g_slots + g_nprobe * 8;
    sl[0] = *w;                                  // Original instruction
    sl[1] = 0x58000050;                          // ldr x16, #8
    sl[2] = 0xD61F0200;                          // br x16
    uint64_t next = at + 4; memcpy(sl + 3, &next, sizeof next);
    uint64_t pg = at & ~(g_slot_size - 1);
    if (mprotect((void *)pg, g_slot_size, PROT_READ | PROT_WRITE)) die("probe mprotect");
    *w = 0xD43E0000;                             // brk #0xf000
    if (mprotect((void *)pg, g_slot_size, PROT_READ | PROT_EXEC)) die("probe text seal failed");
    __builtin___clear_cache((char *)w, (char *)(w + 1));
    g_probe[g_nprobe].at = at; g_probe[g_nprobe].slot = sl; g_probe[g_nprobe].dump = dump; g_probe[g_nprobe].fn = fn; g_probe[g_nprobe].freg = -1; g_probe[g_nprobe].hreg = -1;
    snprintf(g_probe[g_nprobe].name, sizeof g_probe[0].name, "%s+%#llx", m->name, (unsigned long long)off);
    LOG("probe %s (original instruction %08x)", g_probe[g_nprobe].name, sl[0]);
    g_nprobe++;
}

void dm_probe_text_install(void) {
    crash_set_trap_hook(probe_hit);
    const char *frames = getenv("DM_PROBE_FRAMES");
    if (frames) {
        char *end; g_probe_frame_first = strtoull(frames, &end, 10);
        g_probe_frame_last = *end == '-' ? strtoull(end + 1, NULL, 10) : g_probe_frame_first;
    }
    const char *limit = getenv("DM_PROBE_LIMIT");
    if (limit) g_probe_limit = (unsigned)strtoul(limit, NULL, 10);
    if ((limit = getenv("DM_PROBE_EVERY"))) g_probe_every = (unsigned)strtoul(limit, NULL, 10);
    const char *e = getenv("DM_PROBE");
    if (e) {
        char spec[512]; strlcpy(spec, e, sizeof spec);
        for (char *t = strtok(spec, ","); t; t = strtok(NULL, ",")) {
            char *c = strchr(t, ':'); if (!c) die("DM_PROBE format: module:offset");
            *c = 0; module_t *m = NULL;
            for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, t)) m = &g_mod[i];
            if (!m) die("DM_PROBE: no such module %s", t);
            char *at_ = strchr(c + 1, '@');
            long dump = -1, post = 0;
            if (at_) {   // @offset / @*offset / @xN+offset / @*xN+offset / @*xN+offset+post (the dereferenced pointer plus post: @*x0+48+8 = an al::LiveActor's position, al::getTrans)
                const char *q = at_ + 1; long deref = 0, reg = 0; char *e3;
                if (*q == '*') { deref = 0x1000000; q++; }
                if (*q == 'x') { char *e2; reg = strtol(q + 1, &e2, 10) & 31; q = *e2 == '+' ? e2 + 1 : e2; }
                dump = deref | (reg << 25) | (strtol(q, &e3, 16) & 0xffffff);
                if (deref && *e3 == '+') post = strtol(e3 + 1, 0, 16);
            }
            char *qm = strchr(c + 1, '?'), *bang = strchr(c + 1, '!');
            probe_add(m, strtoull(c + 1, 0, 16), dump, NULL);
            g_probe[g_nprobe - 1].dpost = post;
            if (bang && bang[1] == 'x') { g_probe[g_nprobe - 1].hreg = (int)strtol(bang + 2, 0, 10) & 31; probe_heapcfg(); }
            if (qm) {   // Filter: ?x0=706c400000 or ?[x0+38]=781548 (hex)
                const char *q = qm + 1; int br = *q == '[';
                if (br) q++;
                if (*q != 'x') die("DM_PROBE filter format: ?xN=value or ?[xN+offset]=value");
                char *e2; g_probe[g_nprobe - 1].freg = (int)strtol(q + 1, &e2, 10) & 31; g_probe[g_nprobe - 1].foff = -1;
                if (br) { g_probe[g_nprobe - 1].foff = *e2 == '+' ? strtol(e2 + 1, &e2, 16) : 0; if (*e2 == ']') e2++; }
                if (*e2 != '=') die("DM_PROBE filter missing =");
                g_probe[g_nprobe - 1].fval = strtoull(e2 + 1, 0, 16);
            }
        }
    }
    if (g_slots && !g_slots_sealed) {
        if (mprotect(g_slots, g_slot_size, PROT_READ | PROT_EXEC)) die("probe trampoline seal failed");
        __builtin___clear_cache((char *)g_slots, (char *)g_slots + g_slot_size);
        g_slots_sealed = 1;
    }

}
#endif
