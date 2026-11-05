// Real DM_CTL memory helpers: readable boundaries, exact files and paired cvars.
#include "../hle_fpslock.c"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

static module_t modules[1];
module_t *g_mod = modules;
int g_nmod = 1;
static uintptr_t prepared[4];
static unsigned writes;
void dm_guest_write_prepare(void *p, size_t n) {
    assert(n == 4 && writes < 4); prepared[writes++] = (uintptr_t)p;
}
static void file_bytes(const char *path, const void *expected, size_t size) {
    FILE *f = fopen(path, "rb"); assert(f);
    for (size_t i = 0; i < size; i++) assert(fgetc(f) == ((const uint8_t *)expected)[i]);
    assert(fgetc(f) == EOF && !fclose(f));
}
int main(void) {
    size_t pg = (size_t)getpagesize(), data = 65536;
    uint8_t *base = mmap(NULL, data + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(base != MAP_FAILED); for (size_t i = 0; i < data; i++) base[i] = (uint8_t)(i * 19 + 7);
    assert(!mprotect(base + data, pg, PROT_NONE));
    *g_mod = (module_t){.name="main", .base=base, .size=data + pg};
    struct { uint32_t before; float value[2]; uint32_t after; } f = {0x1234, {3, 4}, 0x5678};
    struct { uint32_t before; int32_t value[2]; uint32_t after; } n = {0x4321, {5, 6}, 0x8765};
    *(uintptr_t *)(base + 0x110) = (uintptr_t)f.value;
    *(uintptr_t *)(base + 0x210) = (uintptr_t)n.value;
    dm_ctl_cvar_f32(0x100, 17.5f); dm_ctl_cvar_i32(0x200, -39);
    assert(f.value[0] == 17.5f && f.value[1] == 17.5f && f.before == 0x1234 && f.after == 0x5678);
    assert(n.value[0] == -39 && n.value[1] == -39 && n.before == 0x4321 && n.after == 0x8765);
    assert(writes == 4 && prepared[0] == (uintptr_t)&f.value[0] && prepared[1] == (uintptr_t)&f.value[1]);
    assert(prepared[2] == (uintptr_t)&n.value[0] && prepared[3] == (uintptr_t)&n.value[1]);
    *(uintptr_t *)(base + 0x110) = 0;
    dm_ctl_cvar_f32(0x100, 0); dm_ctl_cvar_i32(data, 0); assert(writes == 4);
    g_nmod = 0; dm_ctl_cvar_f32(0x200, 0); assert(writes == 4); g_nmod = 1;

    char dir[512], path[600];
    const char *tmp = getenv("TMPDIR");
#ifdef __ANDROID__
    if (!tmp) tmp = "/data/local/tmp";
#else
    if (!tmp) tmp = "/tmp";
#endif
    snprintf(dir, sizeof dir, "%s/ctl-memory-XXXXXX", tmp); assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/dump.bin", dir);
    dm_ctl_memdump((uintptr_t)base, data, path); file_bytes(path, base, data);
    dm_ctl_memdump((uintptr_t)base, data + pg, path); file_bytes(path, base, data);
    dm_ctl_memdump((uintptr_t)base + data - 17, 17, path); file_bytes(path, base + data - 17, 17);
    dm_ctl_memdump((uintptr_t)base + data, 1, path); file_bytes(path, base, 0);
    dm_ctl_memdump((uintptr_t)base, 0, path); file_bytes(path, base, 0);
    dm_ctl_memdump((uintptr_t)base, 1, dir); // An unwritable output reports failure and returns.

    FILE *log = tmpfile(); assert(log); int saved = dup(STDERR_FILENO); assert(saved >= 0);
    fflush(stderr); assert(dup2(fileno(log), STDERR_FILENO) == STDERR_FILENO);
    float values[64]; for (unsigned i = 0; i < 64; i++) values[i] = (float)i + 0.5f;
    dm_ctl_peek((uintptr_t)values, 65); // Never read the 65th float.
    dm_ctl_peek((uintptr_t)base + data, 1);
    dm_ctl_peek((uintptr_t)values, -1);
    fflush(stderr); assert(dup2(saved, STDERR_FILENO) == STDERR_FILENO); close(saved);
    assert(!fseek(log, 0, SEEK_SET)); char text[8192]; size_t got = fread(text, 1, sizeof text - 1, log); text[got] = 0; assert(!fclose(log));
    assert(strstr(text, "0.5 1.5 2.5 3.5") && strstr(text, "60.5 61.5 62.5 63.5") && strstr(text, "not readable"));
    unsigned lines = 0; for (size_t i = 0; i < got; i++) lines += text[i] == '\n'; assert(lines == 17);
    assert(!unlink(path) && !rmdir(dir) && !munmap(base, data + pg));
    puts("PASS: control-memory float/int copies, write preparation, bounded peek and exact/partial/rejected dumps");
    return 0;
}
