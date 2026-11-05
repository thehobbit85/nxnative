// NV/VI guest contracts on Mac and Android. No renderer is linked: only the
// frontend's physical display and submitted-frame count are controlled inputs.
#include "../../gfx/hle_nv.c"
#include "../../gfx/hle_vi.c"
#include "../hle_nvdrv.c"
#include <assert.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern int display_width, display_height;
static int sdk = 20, operation_mode;
static uint64_t frames;
int dm_sdk_major(void) { return sdk; }
int dm_opmode(void) { return operation_mode; }
uint64_t dm_nvn_frame_count(void) { return frames; }
static void *seen[64]; static size_t nseen;
static void mark(void *fn) {
    for (size_t i = 0; i < nseen; i++) if (seen[i] == fn) return;
    assert(nseen < 64); seen[nseen++] = fn;
}
#define RUN(fn, ...) (mark((void *)fn), fn(__VA_ARGS__))
static void unchanged(const void *ptr, size_t n) { const uint8_t *p = ptr; for (size_t i = 0; i < n; i++) assert(p[i] == 0xa5); }
static unsigned allocated, freed, resized;
static void *alloc_cb(unsigned long n, unsigned long align, void *user) { assert(n == 64 && align == 16); allocated++; return user; }
static void free_cb(void *p, void *user) { assert(p == user); freed++; }
static void *resize_cb(void *p, unsigned long n, void *user) { assert(p == user && n == 128); resized++; return user; }
static void driver(void) {
    uint8_t pool[64];
    RUN(h_nv_SetAlloc, alloc_cb, free_cb, resize_cb, pool);
    RUN(h_nv_SetDevAlloc, alloc_cb, free_cb, resize_cb, pool);
    struct nv_allocator *a[] = {&g_nv_alloc, &g_nv_devtools_alloc};
    for (unsigned i = 0; i < 2; i++) {
        assert(a[i]->alloc(64, 16, a[i]->user) == pool);
        assert(a[i]->realloc(pool, 128, a[i]->user) == pool); a[i]->free(pool, a[i]->user);
    }
    assert(allocated == 2 && resized == 2 && freed == 2);
    int versions[] = {3, 10, 11, 20};
    for (unsigned i = 0; i < sizeof versions / sizeof *versions; i++) {
        sdk = versions[i]; size_t count = sdk <= 10 ? 4 : 8;
        RUN(h_nv_Fin);
        uint64_t memory[10]; memset(memory, 0xa5, sizeof memory);
        RUN(h_nv_MemInfo, memory + 1);
        unchanged(memory, 8); unchanged(memory + 1 + count, (9 - count) * 8);
        for (size_t k = 0; k < count; k++) assert(!memory[k + 1]);
        RUN(h_nv_Init, pool, 0x180000000ul); RUN(h_nv_InitCheck);
        RUN(h_nv_MemInfo, memory + 1);
        assert(memory[1] == 0x180000000ull && memory[2] == 0 && memory[3] == memory[1]);
        unchanged(memory, 8); unchanged(memory + 1 + count, (9 - count) * 8);
        RUN(h_nv_Dump); RUN(h_nv_Fin);
    }
    pid_t child = fork(); assert(child >= 0);
    if (!child) { struct rlimit lim = {0, 0}; setrlimit(RLIMIT_CORE, &lim); h_nv_InitCheck(); _exit(9); }
    int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    RUN(h_gs_Check, "fixture", 7);
    assert(RUN(h_nvos_Open, "/dev/nvhost-nvdec") == -1 && RUN(h_nvos_Open, NULL) == -1);
    uint8_t bytes[128]; memset(bytes, 0xa5, sizeof bytes);
    for (int fd = -1; fd <= 1; fd++) {
        assert(RUN(h_nvos_Ioctl, fd, 0xffffffffu, bytes + 16, 96) == 3);
        assert(RUN(h_nvos_Ioctl2, fd, 0, bytes + 16, 96) == 3);
        assert(RUN(h_nvos_Ioctl3, fd, 0, NULL, 0) == 3);
        assert(RUN(h_nvos_MapShared, fd) == 3);
        assert(RUN(h_nvos_QueryEvent, fd, 0xffffffffu, bytes + 16) == 3);
        RUN(h_nvos_Close, fd); unchanged(bytes, sizeof bytes);
    }
}
static void display(void) {
    RUN(h_vi_Init);
    struct { uintptr_t before; void *value; uintptr_t after; } out = {123, NULL, 456};
    assert(!RUN(h_vi_OpenDisp, &out.value)); void *display = out.value;
    assert(display && out.before == 123 && out.after == 456);
    assert(!RUN(h_vi_OpenNamedDisp, &out.value, "Default") && out.value == display);
    assert(RUN(h_vi_OpenNamedDisp, &out.value, "Other") == 0x2a2 && out.value == display);
    assert(RUN(h_vi_OpenNamedDisp, &out.value, NULL) == 0x2a2 && out.value == display);
    uint8_t info[0xe0]; memset(info, 0xa5, sizeof info);
    assert(!RUN(h_vi_ListDisp, info + 16, 0) && !RUN(h_vi_ListDisp, NULL, 4)); unchanged(info, sizeof info);
    assert(RUN(h_vi_ListDisp, info + 16, 2) == 1 && !strcmp((char *)info + 16, "Default"));
    assert(info[16 + 0x40] == 1);
    uint64_t limits[3]; memcpy(limits, info + 16 + 0x48, sizeof limits);
    assert(limits[0] == 1 && limits[1] == 1920 && limits[2] == 1080);
    unchanged(info, 16); unchanged(info + 16 + 0x60, sizeof info - 16 - 0x60);
    assert(!RUN(h_vi_CreateLayer, &out.value, display)); void *layer = out.value;
    assert(layer && layer != display && !RUN(h_vi_GetNative, &out.value, layer)); void *window = out.value;
    assert(window && window != layer && !RUN(h_vi_GetNative, &out.value, layer) && out.value == window);
    assert(out.before == 123 && out.after == 456);
    int w, h; dm_vi_layer_size(&w, &h); assert(!w && !h);
    assert(!RUN(h_vi_CreateLayerWH, &out.value, display, 1920, 1080));
    dm_vi_layer_size(&w, &h); assert(w == 1920 && h == 1080 && out.value == layer);
    assert(!RUN(h_vi_CreateLayerWH, &out.value, display, 65536, -1));
    dm_vi_layer_size(&w, &h); assert(w == 1920 && h == 1080);
    int32_t settings[] = {1280, 720, 2};
    assert(!RUN(h_vi_CreateLayerSettings, &out.value, display, settings));
    dm_vi_layer_size(&w, &h); assert(w == 1280 && h == 720);
    assert(!RUN(h_vi_CreateLayerSettings, &out.value, display, NULL));
    int crop[4]; assert(!RUN(h_vi_SetCrop, layer, 11, 13, 640, 360)); dm_vi_layer_crop(crop);
    assert(crop[0] == 11 && crop[1] == 13 && crop[2] == 640 && crop[3] == 360);
    assert(!RUN(h_vi_SetCrop, layer, -1, 0, 0, 720)); dm_vi_layer_crop(crop);
    assert(!crop[0] && !crop[1] && !crop[2] && !crop[3]);
    assert(!RUN(h_vi_SetScaling, layer, 1));
    uint64_t frame[3] = {123, 0, 456}; frames = 0x100000009ull;
    assert(!RUN(h_vi_FrameNo, frame + 1, layer) && frame[1] == frames && frame[0] == 123 && frame[2] == 456);
    memset(info, 0xa5, sizeof info); assert(!RUN(h_vi_FrameHist, info, 2, layer)); unchanged(info, sizeof info);
    assert(!RUN(h_vi_PanelId, info + 16)); unchanged(info, 16); unchanged(info + 40, sizeof info - 40);
    for (int i = 16; i < 40; i++) assert(!info[i]);
    struct { uint8_t on, guard; uint16_t peak; uint32_t format; uint64_t tail; } hdr = {9, 7, 8, 9, 123};
    assert(!RUN(h_vi_HdrStatus, &hdr.on, &hdr.format, &hdr.peak, display));
    assert(!hdr.on && !hdr.peak && !hdr.format && hdr.guard == 7 && hdr.tail == 123);
    assert(!RUN(h_vi_HdrStatus, NULL, NULL, NULL, display));
    assert(!RUN(h_vi_PaperWhite, &hdr.peak, display) && hdr.peak == 203 && hdr.guard == 7);
    assert(!RUN(h_vi_PaperWhite, NULL, display));
    hdr.on = 9; assert(!RUN(h_vi_VrrSupported, &hdr.on, display) && !hdr.on && hdr.guard == 7);
    assert(!RUN(h_vi_VrrSupported, NULL, display) && !RUN(h_vi_SetVrr, display, 1));
    int cases[][3] = {{0, 0, 1}, {1280, 720, 0}, {1920, 1080, 1}, {2560, 1440, 2}, {3840, 2160, 3}, {1920, 1079, 0}, {720, 1280, 0}, {2160, 3840, 3}};
    for (operation_mode = 0; operation_mode <= 1; operation_mode++) for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        display_width = cases[i][0]; display_height = cases[i][1]; uint32_t answer[3] = {123, 99, 456};
        assert(!RUN(h_vi_BufMaxRes, answer + 1, display) && answer[1] == (uint32_t)(operation_mode ? cases[i][2] : 1));
        assert(answer[0] == 123 && answer[2] == 456);
    }
    assert(!RUN(h_vi_BufMaxRes, NULL, display));
    assert(!RUN(h_vi_ExtSwap, layer, 1, 2) && !RUN(h_vi_ExtSwapClear, layer));
    assert(!RUN(h_vi_HdrGamut, layer, 1) && !RUN(h_vi_HdrToneMap, layer, 2));
    assert(!RUN(h_vi_HdrExtLum, layer, 100, 1000) && !RUN(h_vi_HdrStdLum, layer, 100, 1000));
    assert(!RUN(h_vi_HdrBezier, layer, .1f, .5f, .9f) && !RUN(h_vi_HdrBezierClear, layer));
    assert(!RUN(h_vi_HdrGamutClear, layer) && !RUN(h_vi_HdrToneMapClear, layer));
    RUN(h_vi_DestroyLayer, layer); RUN(h_vi_CloseDisp, display); RUN(h_vi_Fini);
    assert(!RUN(h_vi_OpenDisp, &out.value) && out.value == display); RUN(h_vi_CloseDisp, out.value);
}
int main(void) {
    driver(); display();
    unsigned long bytes = 0; const hle_force_ent *entries = DM_SECTION_DATA(swforce, &bytes);
    size_t count = bytes / sizeof *entries;
    for (size_t i = 0; i < count; i++) {
        size_t j; for (j = 0; j < nseen && entries[i].fn != seen[j]; j++) {}
        if (j == nseen) { fprintf(stderr, "untested registration: %s\n", entries[i].name); return 1; }
    }
    assert(count == nseen); printf("display: all %zu NV/NVDRV/VI registrations, ABI guards, handles and display modes passed\n", count); return 0;
}
