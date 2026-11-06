// Synthetic guest for the release-linked Android app. No commercial game data.
// Uses rewritten SVC instructions and relocated NVN imports, including presentation.
#include "nvn_protos.h"
#include <stddef.h>
#include <stdint.h>
extern void *nvnBootstrapLoader(const char *);
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } pad_state;
extern int hid_read(pad_state *, int, const uint32_t *);
extern uint32_t fs_mount(const char *, const void *), fs_open(uint64_t *, const char *, int);
extern uint32_t fs_create(const char *, int64_t), fs_read(size_t *, uint64_t, int64_t, void *, size_t);
extern uint32_t fs_write(uint64_t, int64_t, const void *, size_t, const void *), fs_commit(const char *);
extern void fs_close(uint64_t);
extern int aoc_count(void), font_state(int);
extern const void *font_address(int);
#define API(name) ((NVNT_##name)nvnBootstrapLoader(#name))
void *memset(void *p, int c, size_t n) { unsigned char *b = p; while (n--) *b++ = c; return p; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
static void message(const char *s, size_t n) {
    register const char *x0 __asm__("x0") = s; register size_t x1 __asm__("x1") = n;
    __asm__ volatile("svc #0x27" : "+r"(x0), "+r"(x1) : : "memory");
}
#define NOTE(s) message(s, sizeof(s) - 1)
#ifdef APP_CONTROLLERS
extern int hid_full(pad_state *, int, const uint32_t *);
extern void hid_ids(const uint32_t *, size_t);
static void report_pad(unsigned player, const pad_state *pad) {
    // Connected, sixteen buttons, and which stick axes are positive/negative.
    static uint32_t last[2] = {UINT32_MAX, UINT32_MAX};
    uint32_t bits = (uint32_t)(pad->buttons & 0xffff) | ((pad->attributes & 1) << 16);
    int32_t axes[] = {pad->lx, pad->ly, pad->rx, pad->ry};
    for (unsigned i = 0; i < 4; i++) {
        if (axes[i] > 16000) bits |= 1u << (17 + i * 2);
        if (axes[i] < -16000) bits |= 1u << (18 + i * 2);
    }
    if (bits != last[player]) {
        char line[] = "GUEST_PAD_0=00000000\n";
        line[10] += player;
        for (unsigned i = 0; i < 8; i++) line[19-i] = "0123456789abcdef"[(bits >> (i*4)) & 15];
        message(line, sizeof line - 1);
        last[player] = bits;
    }
}
#endif
static void sleep_frame(void) {
    register long x0 __asm__("x0") = 16666667;
    __asm__ volatile("svc #0x0b" : "+r"(x0) : : "memory");
}
static void require(int ok) { if (!ok) { NOTE("APP_FIXTURE_FAILED\n"); for (;;) sleep_frame(); } }
#ifdef APP_MOTION
#include "app_motion.h"
#endif
#ifdef APP_KEYBOARD
#include "app_keyboard.h"
#endif
#ifdef APP_WATCH
static uintptr_t watch_heap(size_t size) {
    register uint64_t x0 __asm__("x0") = 0; register uint64_t x1 __asm__("x1") = size;
    __asm__ volatile("svc #0x01" : "+r"(x0), "+r"(x1) :: "memory");
    require(!x0); return x1;
}
static uintptr_t watch_region(unsigned id) {
    register uint64_t x0 __asm__("x0") = 0, x1 __asm__("x1") = id;
    register uint64_t x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
    __asm__ volatile("svc #0x29" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    require(!x0); return x1;
}
static void watch_physical(int commit, uintptr_t at) {
    register uint64_t x0 __asm__("x0") = at, x1 __asm__("x1") = 0x4000;
    if (commit) __asm__ volatile("svc #0x2c" : "+r"(x0), "+r"(x1) :: "memory");
    else __asm__ volatile("svc #0x2d" : "+r"(x0), "+r"(x1) :: "memory");
    require(!x0);
}
static void watch_stack_map(int commit, uintptr_t dst, uintptr_t src) {
    register uint64_t x0 __asm__("x0") = dst, x1 __asm__("x1") = src, x2 __asm__("x2") = 0x4000;
    if (commit) __asm__ volatile("svc #0x04" : "+r"(x0), "+r"(x1), "+r"(x2) :: "memory");
    else __asm__ volatile("svc #0x05" : "+r"(x0), "+r"(x1), "+r"(x2) :: "memory");
    require(!x0);
}
static uintptr_t watch_open(void) {
    if (APP_WATCH == 1) return watch_heap(0x200000);
    uintptr_t at = watch_region(2); watch_physical(1, at); return at;
}
static void watch_close(uintptr_t at) {
    if (APP_WATCH == 1) watch_heap(0); else watch_physical(0, at);
}
static void check_watch(void) {
    if (APP_WATCH == 3) {
        uintptr_t heap = watch_heap(0x200000), stack = watch_region(14), at = stack + 0x80;
        for (unsigned i = 0; i < 32; i++) {
            *(volatile uint64_t *)(heap + 0x80) = 0x100 + i;
            watch_stack_map(1, stack, heap); require(*(volatile uint64_t *)at == 0x100 + i);
            uint64_t value = 0xabc000 + i;
            __asm__ volatile("str %1, [%0]" :: "r"(at), "r"(value) : "memory");
            watch_stack_map(0, stack, heap); require(*(volatile uint64_t *)(heap + 0x80) == value);
            sleep_frame(); // exercise guest scheduling while the bounded diagnostic queue drains
        }
        watch_stack_map(1, stack, heap); uint64_t value = 0x7777;
        __asm__ volatile("stlr %1, [%0]" :: "r"(at), "r"(value) : "memory");
        require(*(volatile uint64_t *)at == value); value = 0xbeef;
        __asm__ volatile("str %1, [%0]" :: "r"(at), "r"(value) : "memory");
        watch_stack_map(0, stack, heap); require(*(volatile uint64_t *)(heap + 0x80) == value);
        watch_heap(0); NOTE("WATCH_FIXTURE_DONE\n"); return;
    }
    for (unsigned i = 0; i < 32; i++) {
        uintptr_t base = watch_open(), at = base + 0x80; uint64_t value = 0xabc000 + i;
        if (i & 1) require(*(volatile uint64_t *)at == 0);
        __asm__ volatile("str %1, [%0]" :: "r"(at), "r"(value) : "memory");
        require(*(volatile uint64_t *)at == value); watch_close(base);
    }
    uintptr_t base = watch_open(), at = base + 0x80; uint64_t value = 0x7777;
    __asm__ volatile("stlr %1, [%0]" :: "r"(at), "r"(value) : "memory");
    require(*(volatile uint64_t *)at == value); watch_close(base);
    base = watch_open(); at = base + 0x80; value = 0xbeef;
    __asm__ volatile("str %1, [%0]" :: "r"(at), "r"(value) : "memory");
    require(*(volatile uint64_t *)at == value); watch_close(base);
    NOTE("WATCH_FIXTURE_DONE\n");
}
#endif
#ifdef APP_WEB
#include "app_web.h"
#endif
static void check_save(void) {
    require(fs_mount("save", 0) == 0);
    uint64_t file; uint32_t value[2] = {0x4e585350, 0}; size_t got = 0;
    if (fs_open(&file, "save:/integration.bin", 1) == 0) {
        require(fs_read(&got, file, 0, value, sizeof value) == 0 && got == sizeof value && value[0] == 0x4e585350);
        fs_close(file); NOTE("APP_FIXTURE_SAVE_RETAINED\n");
    } else require(fs_create("save:/integration.bin", sizeof value) == 0);
    value[1]++; require(fs_open(&file, "save:/integration.bin", 2) == 0);
    uint32_t options = 0; require(fs_write(file, 0, value, sizeof value, &options) == 0);
    fs_close(file); require(fs_commit("save") == 0); NOTE("APP_FIXTURE_SAVE_COMMITTED\n");
}
#ifdef APP_PROGRAM
extern void oe_execute(int, const void *, size_t);
extern int oe_previous(void);
extern unsigned oe_parameter(size_t *, void *, size_t);
extern uint32_t fs_rom(const char *, void *, size_t);
static int previous;
static void check_program(void) {
    previous = oe_previous();
    uint8_t param[12]; for (int i = 0; i < 12; i++) param[i] = 0xa5;
    size_t n = 99;
    if (APP_PROGRAM) {
        require(previous == APP_PROGRAM - 1 && oe_parameter(&n, param + 2, 8) && n == 6);
        require(param[0] == 0xa5 && param[1] == 0xa5 && param[8] == 0xa5 && param[11] == 0xa5);
        require(param[2] == 'N' && param[3] == 0 && param[4] == 0xff && param[5] == APP_PROGRAM && param[6] == 4 && param[7] == 9);
        require(!oe_parameter(&n, param, sizeof param) && n == 0);
    } else require((previous == -1 || previous == 2 || previous == 0) && !oe_parameter(&n, param, sizeof param) && n == 0);
    NOTE("PROGRAM_PARAMETERS_READY\n");
    require(fs_rom("rom", NULL, 0) == 0);
    uint64_t file; uint8_t marker = 99; size_t got = 0;
    require(fs_open(&file, "rom:/program.txt", 1) == 0);
    require(fs_read(&got, file, 0, &marker, 1) == 0 && got == 1 && marker == APP_PROGRAM); fs_close(file);
    if (APP_PROGRAM == 0) NOTE("PROGRAM_0_READY\n");
    if (APP_PROGRAM == 1) NOTE("PROGRAM_1_READY\n");
    if (APP_PROGRAM == 2) NOTE("PROGRAM_2_READY\n");
    if (!APP_PROGRAM && previous == 0) NOTE("PROGRAM_HANDOFF_DONE\n");
}
static void next_program(void) {
    uint8_t param[] = {'N', 0, 0xff, APP_PROGRAM + 1, 4, 9};
    if (APP_PROGRAM == 0 && previous == -1) oe_execute(1, param, sizeof param);
    else if (APP_PROGRAM == 1) oe_execute(2, param, sizeof param);
    else if (APP_PROGRAM == 2) oe_execute(0, NULL, 0);
    else if (APP_PROGRAM == 0 && previous == 2) oe_execute(99, param, sizeof param);
    else return;
    NOTE("PROGRAM_EXECUTE_RETURNED\n"); require(0);
}
#endif
void _init_libc0(void) {}
void nnosInitialize(uint64_t main_handle, uint64_t arg) { require(main_handle != 0 && arg == 0); }
void _init_libc1(void) {}
void nninitInitializeSdkModule(void) {}
void _ZN2nn2os11SetHostArgcEi(int argc) { require(argc >= 1); }
void _ZN2nn2os11SetHostArgvEPPc(char **argv) { require(argv && argv[0]); }
void nninitStartup(void) { NOTE("APP_FIXTURE_SDK_READY\n"); }
void _init_libc2(void) {}
static _Alignas(16384) unsigned char storage[1 << 20];
void nnMain(void) {
    NOTE("APP_FIXTURE_MAIN\n");
#ifdef APP_WATCH
    check_watch();
#endif
    check_save();
#ifdef APP_PROGRAM
    check_program();
#endif
    require(aoc_count() == 1);
    require(font_state(0) == 1 && font_address(0) != NULL);
    require(font_state(5) == 1 && font_address(5) != NULL);
    NOTE("APP_FIXTURE_ASSETS_READY\n");
    NVNdevice device; NVNdeviceBuilder db;
    API(nvnDeviceBuilderSetDefaults)(&db); require(API(nvnDeviceInitialize)(&device, &db));
    NVNqueue queue; NVNqueueBuilder qb;
    API(nvnQueueBuilderSetDefaults)(&qb); API(nvnQueueBuilderSetDevice)(&qb, &device);
    require(API(nvnQueueInitialize)(&queue, &qb));
    NVNmemoryPool pool; NVNmemoryPoolBuilder pb;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, storage, sizeof storage);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    require(API(nvnMemoryPoolInitialize)(&pool, &pb));
    NVNtexture tex[3]; NVNtexture *textures[] = {&tex[0], &tex[1], &tex[2]}; NVNtextureBuilder tb;
    API(nvnTextureBuilderSetDefaults)(&tb); API(nvnTextureBuilderSetDevice)(&tb, &device);
    API(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); API(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    API(nvnTextureBuilderSetSize2D)(&tb, 64, 64);
    require(API(nvnTextureBuilderGetStorageSize)(&tb) <= 32768);
    for (int i = 0; i < 3; i++) {
        API(nvnTextureBuilderSetStorage)(&tb, &pool, 65536 + i * 32768);
        require(API(nvnTextureInitialize)(&tex[i], &tb));
    }
    NVNwindow window; NVNwindowBuilder wb;
    API(nvnWindowBuilderSetDefaults)(&wb); API(nvnWindowBuilderSetDevice)(&wb, &device);
    API(nvnWindowBuilderSetTextures)(&wb, 3, textures);
    API(nvnWindowBuilderSetPresentInterval)(&wb, 1);
    require(API(nvnWindowInitialize)(&window, &wb));
    NVNcommandBuffer cb; require(API(nvnCommandBufferInitialize)(&cb, &device));
#ifdef APP_CONTROLLERS
    uint32_t ids[] = {32, 0, 1};
    hid_ids(ids, 3);
#endif
    uint64_t last_buttons = 0;
    for (unsigned frame = 0;; frame++) {
#ifdef APP_MOTION
        if (!(frame % 10)) report_motion();
#endif
#ifdef APP_KEYBOARD
        if (frame == 10) check_keyboard();
#endif
        pad_state state; uint32_t id = 32;
        require(hid_read(&state, 1, &id) == 1);
        if (state.buttons && state.buttons != last_buttons) NOTE("APP_FIXTURE_BUTTON_DOWN\n");
        if (!state.buttons && last_buttons) NOTE("APP_FIXTURE_BUTTON_UP\n");
        last_buttons = state.buttons;
#ifdef APP_CONTROLLERS
        report_pad(0, &state);
        uint32_t second = 1;
        hid_full(&state, 1, &second);
        report_pad(1, &state);
#endif
        int index = 0; API(nvnQueueAcquireTexture)(&queue, &window, &index);
        API(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0, 65536);
        API(nvnCommandBufferBeginRecording)(&cb);
        const NVNtexture *target = textures[index];
        API(nvnCommandBufferSetRenderTargets)(&cb, 1, &target, 0, 0, 0);
        float color[] = {0.125f, 0.75f, 0.25f, 1};
        API(nvnCommandBufferClearColor)(&cb, 0, color, 15);
        NVNcommandHandle handle = API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(&queue, 1, &handle);
        API(nvnQueuePresentTexture)(&queue, &window, index);
        API(nvnQueueFinish)(&queue);
        if (frame % 60 == 0) NOTE("APP_FIXTURE_FRAME\n");
#ifdef APP_WEB
        if (frame == 60) check_web();
#endif
#ifdef APP_DISPLAY
        if (frame == 60) {
            extern uint32_t vi_resolution(uint32_t *, const void *);
            uint32_t result[3] = {123, 99, 456};
            require(!vi_resolution(result + 1, NULL) && result[0] == 123 && result[2] == 456 && result[1] < 4);
            const char *names[] = {"DISPLAY_MODE_0\n", "DISPLAY_MODE_1\n", "DISPLAY_MODE_2\n", "DISPLAY_MODE_3\n"};
            message(names[result[1]], 15);
        }
#endif
#ifdef APP_PROGRAM
        if (frame == 90) next_program();
#endif
        sleep_frame();
    }
}
