// Real map_image and NRO reset/reload with a watched initialized data page.
#define main nro_fixture_main
#include "t_nro.c"
#undef main
#include "../watch_android.c"

static int (*loader_fault)(void *, int, void *);
static void (*loader_prepare)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { loader_fault = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { loader_prepare = fn; }
void dm_guest_write_prepare(void *p, size_t n) { if (loader_prepare) loader_prepare(p, n); }
uint32_t thr_cur_handle(void) { return 0x1001; }
static void fault(int sig, siginfo_t *info, void *uc) {
    if (loader_fault && loader_fault(info->si_addr, (uc_esr(uc) >> 6) & 1, uc)) return;
    _exit(128 + sig);
}
static void protection(uintptr_t p, const char *want) {
    FILE *f = fopen("/proc/self/maps", "r"); assert(f);
    char line[512], perm[5]; unsigned long lo, hi; int found = 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) == 3 && p >= lo && p < hi) {
        assert(!strncmp(perm, want, 3)); found = 1;
    }
    fclose(f); assert(found);
}
int main(int argc, char **argv) {
    alarm(20); assert(argc == 2);
    int far = !strcmp(argv[1], "far") || !strcmp(argv[1], "back");
    size_t pre = far ? 0x4000 : 0, total = pre + 0x10000;
    g_jit_base = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(g_jit_base != MAP_FAILED);
    g_njit_mod = 1; strcpy(g_jit_mod[0].name, "main");
    g_jit_mod[0].off = pre; g_jit_mod[0].pre = pre; g_jit_mod[0].size = 0x10000;
    allocate_modules(1, NULL); g_nmod = 1; module_t *m = g_mod;
    *m = (module_t){.name="main", .text_end=0x4000, .ro_off=0x4000, .data_off=0x4000,
        .key_page_off=far ? 0xc000 : 0x8000, .key_page_size=0x4000, .patch_off=far ? -0x4000 : 0xc000,
        .image_size=0x10000, .file_image_len=0x5000, .nro=1, .pristine_len=0x2000};
    for (unsigned i = 0; i < 4; i++) m->key_idx[i] = i;
    uintptr_t at = (uintptr_t)g_jit_base + pre + 0x4080;
    int bss = !strcmp(argv[1], "bss");
    if (bss) at += 0x2000;
    if (!strcmp(argv[1], "code")) at -= 0x4000;
    if (!strcmp(argv[1], "key")) at = (uintptr_t)g_jit_base + pre + m->key_page_off;
    if (!strcmp(argv[1], "patch")) at = (uintptr_t)g_jit_base + pre + m->patch_off;
    if (!strcmp(argv[1], "back")) { m->back_off = 0x8000; m->back_len = 0x4000; at = (uintptr_t)g_jit_base + pre + m->back_off; }
    char config[64];
    if (!strcmp(argv[1], "relative")) snprintf(config, sizeof config, "main:4080+8");
    else if (!strcmp(argv[1], "missing")) snprintf(config, sizeof config, "absent:80+8");
    else if (!strcmp(argv[1], "overflow")) snprintf(config, sizeof config, "main:fffffffffffffff0+8");
    else snprintf(config, sizeof config, "%llx+8", (unsigned long long)at);
    assert(!setenv("DM_PROBE_WATCH", config, 1));
    char dir[512], path[640]; snprintf(dir, sizeof dir, "%s/watch-loader-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/data/local/tmp");
    assert(mkdtemp(dir)); snprintf(path, sizeof path, "%s/main.patched.bin", dir);
    size_t file_size = far ? 0x9000 + m->back_len : 0x10000;
    uint8_t *file = calloc(1, file_size); assert(file);
    uint64_t initial = UINT64_C(0x1234567890abcdef); memcpy(file + 0x4080, &initial, 8);
    FILE *f = fopen(path, "wb"); assert(f && fwrite(file, 1, file_size, f) == file_size); assert(!fclose(f)); free(file);
    map_image(m, dir);
    assert(!unlink(path) && !rmdir(dir));
    assert(m->base == g_jit_base + pre && *(uint64_t *)(m->base + 0x4080) == initial);
    protection((uintptr_t)m->base, "r-x"); protection((uintptr_t)m->base + m->key_page_off, "r--");
    protection((uintptr_t)m->base + m->patch_off, "r-x");
    uint8_t *alt = malloc(65536); assert(alt); stack_t ss = {.ss_sp=alt, .ss_size=65536}; assert(!sigaltstack(&ss, NULL));
    struct sigaction sa = {.sa_sigaction=fault, .sa_flags=SA_SIGINFO | SA_ONSTACK}; sigemptyset(&sa.sa_mask); assert(!sigaction(SIGSEGV, &sa, NULL));
    dm_android_watch_install(); protection(at, "r--");
    m->dyn = (const Elf64_Dyn *)(m->base + 0x4000); // an empty, real mapped DT_NULL table
    for (unsigned i = 0; i < 32; i++) {
        nro_load(m); assert(*(uint64_t *)at == (bss ? 0 : initial) && *(uint64_t *)(m->base + 0x6000) == 0);
        uint64_t v = i + 1; __asm__ volatile("str %1, [%0]" :: "r"(at), "r"(v) : "memory");
        *(uint64_t *)(m->base + 0x6000) = 0xbeef;
        assert(*(uint64_t *)at == v); protection(at, "r--"); nro_unload(m);
    }
    assert(g_watch_page.generation == 1 && !atomic_load(&g_watch_disarmed));
    protection((uintptr_t)m->base, "r-x"); protection((uintptr_t)m->base + m->key_page_off, "r--");
    protection((uintptr_t)m->base + m->patch_off, "r-x");
    alarm(0); puts("PASS: actual module mapping, initialized data, sealed text/key/patch and 32 NRO reset/reload lifetimes under watch");
    return 0;
}
