// DM_HLECOUNT=<output file, may contain %d = process id> (host diagnostic, for HLE coverage audit): count how many times each HLE is called by the guest and write a TSV "category name count" on exit (including 0 counts).
// Method: at load time, resolve guest references to host implementations (forced takeover / HLE table / libc passthrough / unimplemented stubs) to a counting stub:
//   increment the per-symbol counter atomically, then tail-call the original implementation
// Touches only x16 / x17, which may be clobbered between calls; arguments and return values pass through unchanged. Stubs are written in a writable page and sealed read-only executable when loading ends (hlecount_seal).
// svc is counted separately by number (the hle_svc.c dispatch entry calls hlecount_svc). Unavailable on iOS (code written at runtime cannot execute).
#include "dm.h"
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if !TARGET_OS_IPHONE
#define HC_MAX 8192
// The two pointer literals immediately follow the instructions. Apple ARM64 has
// LSE; Android's ARMv8.0 baseline uses an exclusive loop, saving its scratch x15.
#ifdef __APPLE__
static const uint32_t hc_code[] = {
    0x580000d0, 0xd2800031, 0xf831021f,  // ldr x16,counter; mov x17,1; stadd x17,[x16]
    0x580000b0, 0xd61f0200, 0xd503201f,  // ldr x16,implementation; br x16; nop
};
#else
static const uint32_t hc_code[] = {
    0xf81f0fef, 0x58000130,              // str x15,[sp,#-16]!; ldr x16,counter
    0xc85f7e11, 0x91000631,              // retry: ldxr x17,[x16]; add x17,x17,1
    0xc80f7e11, 0x35ffffaf,              // stxr w15,x17,[x16]; cbnz w15,retry
    0xf84107ef, 0x580000b0,              // ldr x15,[sp],#16; ldr x16,implementation
    0xd61f0200, 0xd503201f,              // br x16; nop
};
#endif
typedef struct { uint32_t code[sizeof hc_code / sizeof *hc_code]; _Atomic uint64_t *counter; void *fn; } count_stub;
static struct { const char *kind, *name; void *fn; _Atomic uint64_t n; } g_hc[HC_MAX];
static unsigned g_nhc; static count_stub *g_hcode; static int g_hon = -1, g_hsealed; static const char *g_hpath;
static _Atomic uint64_t g_hsvc[256];
static int hc_on(void) { if (g_hon < 0) { g_hpath = getenv("DM_HLECOUNT"); g_hon = g_hpath != NULL; } return g_hon; }
static void hc_write(void) {
    char pth[1024]; snprintf(pth, sizeof pth, g_hpath, (int)getpid());   // %d in the path is replaced with the process id (parallel runs each write their own copy)
    FILE *f = fopen(pth, "w"); if (!f) { LOG("[RED] DM_HLECOUNT cannot write %s", pth); return; }
    fprintf(f, "kind\tname\tcount\n");
    for (unsigned i = 0; i < g_nhc; i++) fprintf(f, "%s\t%s\t%llu\n", g_hc[i].kind, g_hc[i].name, (unsigned long long)atomic_load(&g_hc[i].n));
    for (int k = 0; k < 256; k++) if (atomic_load(&g_hsvc[k])) fprintf(f, "SVC\t%#x\t%llu\n", k, (unsigned long long)atomic_load(&g_hsvc[k]));
    fclose(f); LOG("DM_HLECOUNT: %u entries → %s", g_nhc, pth);
}
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    if (!hc_on() || !fn) return fn;
    if (g_hsealed) { static int once; if (!once++) LOG("[RED] DM_HLECOUNT: resolution after the pages were sealed (%s), later entries are not counted", name); return fn; }
    for (unsigned i = 0; i < g_nhc; i++) if (g_hc[i].fn == fn && !strcmp(g_hc[i].name, name)) return &g_hcode[i];   // Same name and same implementation reuse one stub
    if (g_nhc == HC_MAX) die("DM_HLECOUNT more than %d entries", HC_MAX);
    if (!g_hcode) { g_hcode = mmap(NULL, HC_MAX * sizeof *g_hcode, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (g_hcode == MAP_FAILED) die("DM_HLECOUNT mmap"); atexit(hc_write); }
    unsigned i = g_nhc++; g_hc[i].kind = kind; g_hc[i].name = name; g_hc[i].fn = fn;
    count_stub *stub = &g_hcode[i];
    memcpy(stub->code, hc_code, sizeof hc_code);
    stub->counter = &g_hc[i].n;
    stub->fn = fn;
    return stub;
}
void hlecount_seal(void) {
    if (!hc_on() || !g_hcode) return;
    if (mprotect(g_hcode, HC_MAX * sizeof *g_hcode, PROT_READ | PROT_EXEC)) die("DM_HLECOUNT mprotect");
    g_hsealed = 1;
    __builtin___clear_cache((char *)g_hcode, (char *)(g_hcode + g_nhc));
    LOG("DM_HLECOUNT: %u counting stubs sealed", g_nhc);
}
void hlecount_svc(int num) { if (g_hon > 0 && num >= 0 && num < 256) atomic_fetch_add(&g_hsvc[num], 1); }
void hlecount_flush(void) { if (hc_on() && g_hcode) hc_write(); }   // Also write a copy on the graceful exit path (SIGTERM / scene disconnect)
#else
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
void hlecount_seal(void) {}
void hlecount_svc(int num) { (void)num; }
void hlecount_flush(void) {}
#endif
