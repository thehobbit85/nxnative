// Shared-font file/resource contracts and SDK work-buffer ownership. Small
// synthetic byte files exercise transport only; font parsing stays in the SDK.
#include "../hle_sdk16.c"
#include <assert.h>

module_t *g_mod; int g_nmod; uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
static int sdk_major = 20, decode_import = 1;
int dm_sdk_major(void) { return sdk_major; }
int dm_game_imports(const char *prefix) { assert(!strcmp(prefix, "_ZN2nn6fontll24ScalableFontEngineHelper6Decode")); return decode_import; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; abort(); }
void dm_applet_enter(const char *name) { (void)name; abort(); }
void dm_applet_leave(void) { abort(); }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; abort(); }
void hle_signal_system_event(void *ev) { (void)ev; abort(); }
uint64_t hle_system_event_binding(void *ev) { (void)ev; abort(); }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; abort(); }
uint32_t hle_hid_connected_npad(void) { return 0; }
#ifdef __APPLE__
void dm_host_helper_qos(qos_class_t q) { (void)q; }
#endif
typedef struct { void *buffer; unsigned size, calls; } engine;
static void sdk_init(void *self, void *buffer, unsigned size) { engine *e = self; e->buffer = buffer; e->size = size; e->calls++; }
static void *sdk_work(void *self) { return ((engine *)self)->buffer; }
void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn6fontll18ScalableFontEngine10InitializeEPvj")) return sdk_init;
    if (!strcmp(name, "_ZN2nn6fontll18ScalableFontEngine22GetPointerToWorkBufferEv")) return sdk_work;
    fprintf(stderr, "unexpected SDK lookup: %s\n", name); abort();
}
static void unchanged(const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) assert(b[i] == 0xa5); }
static void engines(void) {
    engine e = {0}; uint8_t small[16], other[16], large[16]; // Oracles never dereference the work buffers.
    memset(small, 0xa5, sizeof small); memset(other, 0xa5, sizeof other); memset(large, 0xa5, sizeof large);
    h_font_Initialize(&e, small, 16); assert(e.buffer != small && e.size == 0x80000 && e.calls == 1);
    void *host = e.buffer; assert(h_font_WorkBuffer(&e) == small);
    h_font_Initialize(&e, other, 8); assert(e.buffer == host && e.size == 0x80000 && e.calls == 2);
    assert(h_font_WorkBuffer(&e) == other);
    // The game finalizes/reinitializes the same engine with adequate memory.
    // It must get that new buffer back, not a previously freed small buffer.
    h_font_Initialize(&e, large, 0x80000); assert(e.buffer == large && e.size == 0x80000 && e.calls == 3);
    assert(h_font_WorkBuffer(&e) == large);
    h_font_Initialize(&e, small, 1); assert(e.buffer == host && e.size == 0x80000 && h_font_WorkBuffer(&e) == small);
    engine other_engine = {0}; h_font_Initialize(&other_engine, other, 0x100000);
    assert(other_engine.buffer == other && other_engine.size == 0x100000 && h_font_WorkBuffer(&other_engine) == other);
    h_font_Initialize(&other_engine, other, 8);
    assert(other_engine.buffer != host && other_engine.buffer != other && h_font_WorkBuffer(&other_engine) == other);
    void *other_host = other_engine.buffer;
    h_font_Initialize(&e, large, 0x80001);
    assert(h_font_WorkBuffer(&e) == large && h_font_WorkBuffer(&other_engine) == other && other_engine.buffer == other_host);
    unchanged(small, sizeof small); unchanged(other, sizeof other); unchanged(large, sizeof large);
    puts("PASS: font engine buffer ownership across small/large reinitialization and independent engines");
}
static void put_file(const char *path, const void *data, size_t size) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, data, size) == (ssize_t)size && !close(fd));
}
static void resources(const char *mode) {
    int extension = strcmp(mode, "fallback") != 0;
    if (!strcmp(mode, "old")) sdk_major = 8;
    if (!strcmp(mode, "ttf")) decode_import = 0;
    char dir[1024], standard[1100], alternate[1100], extended[1100];
    const char *tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
    assert(snprintf(dir, sizeof dir, "%s/dm-font-XXXXXX", tmp) < (int)sizeof dir && mkdtemp(dir));
    snprintf(standard, sizeof standard, "%s/standard.ttf", dir); snprintf(alternate, sizeof alternate, "%s/override.ttf", dir);
    snprintf(extended, sizeof extended, "%s/nintendo_ext.ttf", dir);
    const uint8_t bytes[] = {0,1,0,0,0x55,0xaa,0x13,0x37,0x17};
    const uint8_t ext[] = {0x4f,0x54,0x54,0x4f,0,7};
    put_file(standard, "directory candidate", 19); put_file(alternate, bytes, sizeof bytes);
    if (extension) put_file(extended, ext, sizeof ext);
    setenv("DM_FONT_DIR", dir, 1); setenv("DM_SHARED_FONT", alternate, 1); unsetenv("DM_SHARED_FONT_EXT");
    size_t skip = (sdk_major <= 10 || !decode_import) ? 8 : 0;
#ifdef DM_GAME_SHARED_FONT_TTF
    skip = DM_GAME_SHARED_FONT_TTF ? 8 : 0;
#endif
    for (int type = 0; type < 6; type++) {
        h_pl_ReqLoad(type); assert(h_pl_LoadState(type) == 1);
        const uint8_t *payload = type == 5 && extension ? ext : bytes;
        size_t n = type == 5 && extension ? sizeof ext : sizeof bytes;
        const uint8_t *p = h_pl_Addr(type); assert(p && h_pl_Addr(type) == p && h_pl_Size(type) == n + 8 - skip);
        const uint8_t *base = p - skip; uint32_t a, b; memcpy(&a, base, 4); memcpy(&b, base + 4, 4);
        assert(a == 0x18029a7f && __builtin_bswap32(b) == (n ^ 0x49621806u));
        assert(!memcmp(base + 8, payload, n));
        struct { uint64_t pre; pl_builtin_font_t font; uint64_t post; } g;
        for (unsigned api = 0; api < 2; api++) {
            memset(&g, 0xa5, sizeof g);
            assert(api ? h_pl_BuiltInNx(&g.font, type, 0) : h_pl_BuiltInOunce(&g.font, type, 1));
            assert(g.pre == 0xa5a5a5a5a5a5a5a5ull && g.post == g.pre && g.font.type == type && !g.font.flag);
            assert(g.font.address == p && g.font.size == n + 8 - skip);
            for (unsigned i = 0; i < sizeof g.font.pad; i++) assert(!g.font.pad[i]);
        }
    }
    _Static_assert(sizeof(pl_font_data_t) == 24 && sizeof(pl_builtin_font_t) == 24, "font resource ABI");
    for (int max = -1; max <= 7; max++) {
        struct { uint64_t pre; pl_font_data_t font[7]; uint64_t post; } g; memset(&g, 0xa5, sizeof g);
        int count[3] = {123,-1,456}; assert(h_pl_GetSharedFont(count + 1, g.font, max, 0) && count[1] == 6);
        assert(count[0] == 123 && count[2] == 456 && g.pre == 0xa5a5a5a5a5a5a5a5ull && g.post == g.pre);
        int written = max < 0 ? 0 : max < 6 ? max : 6;
        for (int i = 0; i < written; i++) {
            const uint8_t *p = i == 5 && extension ? ext : bytes; size_t n = i == 5 && extension ? sizeof ext : sizeof bytes;
            assert(g.font[i].type == (unsigned)i && !g.font[i].pad && g.font[i].size == n && !memcmp(g.font[i].address, p, n));
        }
        unchanged(g.font + written, sizeof g.font - written * sizeof *g.font);
    }
    assert(!h_pl_IsOunce() && !h_pl_IsVr() && h_pl_BeginVr() == 0xd08e); h_pl_EndVr(); assert(!h_pl_IsVr());
    assert(!unlink(standard) && !unlink(alternate)); if (extension) assert(!unlink(extended)); assert(!rmdir(dir));
    assert(h_pl_LoadState(0) && !memcmp((const uint8_t *)h_pl_Addr(0) + 8 - skip, bytes, sizeof bytes));
    puts("PASS: explicit font override, directory extension/fallback, BFTTF/TTF views, 24-byte resource ABI, bounded priority output and cached lifetime");
}
int main(int argc, char **argv) { assert(argc == 2); resources(argv[1]); engines(); return 0; }
