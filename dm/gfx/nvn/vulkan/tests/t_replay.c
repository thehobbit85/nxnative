// Submit real NVN records to the Vulkan driver, wait for completion and verify CPU-visible pixels.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include <assert.h>
#include <time.h>

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
static prog_t program = {.nstage = 2}, numeric_program = {.nstage = 2}, compute = {.nstage = 1}, groups = {.nstage = 1};
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) {
    if (p == &compute || p == &groups) { assert(stage == 0); memset(key, 0, 8); key[7] = p == &groups ? 4 : 3; return 1; }
    assert((p == &program || p == &numeric_program) && stage < 2);
    memset(key, 0, 8); key[7] = (uint8_t)((p == &numeric_program ? 0x21 : 1) + stage); return 1;
}
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { abort(); }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { abort(); }

static void clear_and_readback(void) {
    tex_t tex = {.serial = 1, .d = {.width = 8, .height = 8, .depth = 1, .levels = 1,
                                    .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&tex.d, &tex.L);
    uint8_t pixels[8 * 8 * 4] = {0};
    struct { op_hdr h; p_clear_tex p; } clear = {
        .h = {OP_CLEAR_TEX, sizeof clear}, .p = {.tex = &tex, .r = {.width = 8, .height = 8, .depth = 1}, .mask = 15, .c.f = {1, 0, 1, 1}}};
    struct { op_hdr h; p_copy_b2t p; } copy = {
        .h = {OP_COPY_T2B, sizeof copy}, .p = {.tex = &tex, .src = (uintptr_t)pixels, .r = {.width = 8, .height = 8, .depth = 1}}};
    struct { op_hdr barrier; typeof(clear) clear; typeof(copy) copy; op_hdr end; } list = {
        .barrier = {OP_BARRIER, sizeof(op_hdr)}, .clear = clear, .copy = copy, .end = {OP_END, sizeof(op_hdr)}};
    mtl_sub_begin();
    mtl_exec((const uint8_t *)&list);
    uint64_t fence = mtl_sub_commit();
    assert(mtl_wait(fence, UINT64_MAX));
    for (size_t i = 0; i < sizeof pixels; i += 4)
        assert(pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 255 && pixels[i + 3] == 255);
    puts("NVN clear, texture readback and Vulkan fence returned 64 expected pixels");
}

typedef struct { _Alignas(8) uint8_t bytes[4096]; size_t length; } list_t;
static void put(list_t *list, unsigned op, const void *payload, size_t bytes) {
    op_hdr header = {op, (uint32_t)((sizeof header + bytes + 7) & ~(size_t)7)};
    assert(list->length + header.size <= sizeof list->bytes);
    memcpy(list->bytes + list->length, &header, sizeof header);
    if (bytes) memcpy(list->bytes + list->length + sizeof header, payload, bytes);
    list->length += header.size;
}
#define RECORD(op, type, ...) put(&list, op, &(type){__VA_ARGS__}, sizeof(type))

static void textured_draw(int compact, int split_submit) {
    list_t list = {0};
    tex_t faces = {.serial = 10 + 2 * compact + 100 * split_submit, .d = {.width = 8, .height = 8, .depth = 6, .levels = 1,
                   .target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8, .flags = NVN_TEXTURE_FLAGS_IMAGE}};
    tex_t output = {.serial = faces.serial + 1, .d = {.width = 8, .height = 8, .depth = 1, .levels = 1,
                    .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&faces.d, &faces.L); nvn_tex_layout(&output.d, &output.L);
    unsigned slot = compact ? 7 : 5;
    view_t cube = {.set_target = 1, .target = NVN_TEXTURE_TARGET_CUBEMAP, .set_layers = 1, .nlayers = 6};
    mtl_reg_tex(slot, &faces, &cube);
    tex_t other = faces; other.serial += 32;
    mtl_reg_tex(slot + 17, &other, NULL);
    mtl_reg_tex(slot + 16, &faces, NULL);  // the same six faces, exposed as a storage image array
    float positions[] = {-1,-1, 3,-1, -1,3};
    float color[] = {compact ? 1.f : 0.f, compact ? 0.f : 1.f, 1.f, 1.f};
    uint8_t pixels[8 * 8 * 4] = {0}, second[sizeof pixels] = {0};
    RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &faces, .r = {.width = 8, .height = 8, .depth = 6}, .mask = 15, .c.f = {0, 0, 1, 1});
    // The first dispatch reads CPU counts. It writes either all three counts for the
    // next dispatch, or only X, leaving Y and Z in guest memory. Both execute before the fence.
    uint32_t count_command[] = {compact ? 1u : 3u, 1, 1};
    uint32_t image_groups[] = {0, compact ? 8u : 0u, compact ? 6u : 0u};
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &groups, .stages = 0x20);
    RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)image_groups,
           .size = count_command[0] * sizeof(uint32_t));
    if (compact) RECORD(OP_DISPATCH, p_dispatch, .x = 1, .y = 1, .z = 1);
    else RECORD(OP_DISPATCH, p_dispatch, .ind = (uintptr_t)count_command);
    if (split_submit) {
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin();
        mtl_exec(list.bytes);
        mtl_sub_commit();
        list.length = 0;
    }
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &compute, .stages = 0x20);
    RECORD(OP_BIND_IMG, p_bind_handles, .stage = 5, .first = 1, .n = 2, .h = {slot + 16, slot + 17});
    RECORD(OP_DISPATCH, p_dispatch, .ind = (uintptr_t)image_groups);
    RECORD(OP_BARRIER, uint32_t, 0);
    RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &program, .stages = 3);
    va_st va = {.fmt = NVN_FORMAT_RG32F};
    p_bind_vattr attr = {.n = 1};
    memcpy(attr.st, &va, sizeof va);
    put(&list, OP_BIND_VATTR, &attr, sizeof attr);
    RECORD(OP_BIND_VSTREAM, p_bind_vstream, .n = 1, .st = {{.stride = 8}});
    if (compact) {
        RECORD(OP_BIND_VBUF1, p_bind_vbuf1, .index = 0, .size = sizeof positions, .addr = (uintptr_t)positions);
        RECORD(OP_BIND_UBO_SMALL, p_bind_ubo_small, .stage = 1, .index = 2, .size = sizeof color, .addr = (uintptr_t)color);
        RECORD(OP_BIND_TEX1, p_bind_tex1, .stage = 1 | BIND_TEX1_NOBASE, .slot = 3, .h = slot);
    } else {
        RECORD(OP_BIND_VBUF, p_bind_vbuf, .first = 0, .n = 1, .r = {{(uintptr_t)positions, sizeof positions}});
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 1, .index = 2, .addr = (uintptr_t)color, .size = sizeof color);
        RECORD(OP_BIND_TEX, p_bind_handles, .stage = 1, .first = 3, .n = 1, .h = {slot});
    }
    RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)pixels, .r = {.width = 8, .height = 8, .depth = 1});
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &other, .src = (uintptr_t)second, .r = {.width = 8, .height = 8, .depth = 1});
    put(&list, OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(list.bytes);
    assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    assert(image_groups[0] == 8 && image_groups[1] == 8 && image_groups[2] == 6);
    const uint8_t expected[] = {compact ? 255 : 0, compact ? 0 : 255, 0, 255};
    for (size_t i = 0; i < sizeof pixels; i += 4) {
        const uint8_t cyan[] = {0, 255, 255, 255};
        assert(!memcmp(second + i, cyan, 4));
        if (memcmp(pixels + i, expected, 4)) {
            fprintf(stderr, "binding form %d, pixel %zu: %u %u %u %u\n", compact, i / 4, pixels[i], pixels[i+1], pixels[i+2], pixels[i+3]);
            abort();
        }
    }
}

static void strided_layers(void) {
    list_t list = {0};
    tex_t tex = {.serial = 91, .d = {.width = 8, .height = 8, .depth = 3, .levels = 1,
                                    .target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&tex.d, &tex.L);
    uint8_t bytes[128], expected[sizeof bytes];
    memset(bytes, 0xcc, sizeof bytes); memset(expected, 0xcc, sizeof expected);
    RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &tex, .r = {.width=8, .height=8, .depth=3}, .mask=15, .c.f={0,0,1,1});
    RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &tex, .r = {.zoffset=1, .width=8, .height=8, .depth=1}, .mask=15, .c.f={1,0,0,1});
    RECORD(OP_COPY_STRIDE, p_copy_stride, .stride=16);
    RECORD(OP_COPY_STRIDE, p_copy_stride, .stride=64, .image=1);
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&tex, .src=(uintptr_t)bytes, .has_view=1, .view={.set_layers=1, .min_layer=1, .nlayers=2},
           .r={.xoffset=1, .yoffset=2, .width=2, .height=3, .depth=2});
    put(&list, OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    for (int layer = 0; layer < 2; ++layer) for (int y = 0; y < 3; ++y) for (int x = 0; x < 2; ++x) {
        uint8_t color[4] = {layer ? 0 : 255, 0, layer ? 255 : 0, 255};
        memcpy(expected + 64 * layer + 16 * y + 4 * x, color, 4);
    }
    assert(!memcmp(bytes, expected, sizeof bytes));
}
// The shader reports all four fetched register words. Green means a faithful fetch,
// red a wrong value and blue that the draw was rejected before pipeline creation.
static void vertex_fetch(void) {
    static const struct {
        NVNformat format; unsigned location; uint32_t data[4], expected[4]; int reject; NVNformat second_format;
    } cases[] = {
        {NVN_FORMAT_R8, 0, {255}, {0x3f800000, 0, 0, 0x3f800000}},
        {NVN_FORMAT_R16I, 2, {0xff80}, {0xffffff80, 0, 0, 1}},
        {NVN_FORMAT_R32I, 1, {0xffff1234}, {0xffff1234, 0, 0, 1}},
        {NVN_FORMAT_RGBA32F, 2, {0x3fc00000, 0xc0100000, 0, 0x3f000000},
                                {0x3fc00000, 0xc0100000, 0, 0x3f000000}},
        {NVN_FORMAT_RGBA32UI, 0, {0x3fc00000, 0xc0100000, 0, 0x3f000000},
                                 {0x3fc00000, 0xc0100000, 0, 0x3f000000}},
        {NVN_FORMAT_R8, 1, {255}, {0x3f800000, 0, 0, 0x3f800000}},
        {NVN_FORMAT_RG32F, 1, {0x3f800000, 0x40000000}, {0x3f800000, 0x40000000, 0, 0x3f800000}},
        {NVN_FORMAT_R16I, 1, {0xff80}, {0xffffff80, 0, 0, 1}},
        {NVN_FORMAT_RG16F, 2, {0xc4003e00}, {0x3fc00000, 0xc0800000, 0, 0x3f800000}},
        {NVN_FORMAT_RG8SN, 1, {0x7f80}, {0xbf800000, 0x3f800000, 0, 0x3f800000}},
        {NVN_FORMAT_R16I, 0, {0xff80}, {0xffffff80, 0, 0, 1}},
        // Two different source classes in one pipeline. The shader XORs both fetches.
        {NVN_FORMAT_RG16F, 1, {0xc4003e00}, {0x3fc03e00, 0xc0800000, 0, 0x3f800001}, 0, NVN_FORMAT_R16I},
        {NVN_FORMAT_R32_UI2F, 0, {3}, {0x40400000, 0, 0, 0x3f800000}},
        {NVN_FORMAT_R32_I2F, 1, {(uint32_t)-16777217}, {0xcb800000, 0, 0, 0x3f800000}},
        {NVN_FORMAT_RG32_UI2F, 2, {16777217, 16777219}, {0x4b800000, 0x4b800002, 0, 0x3f800000}},
        {NVN_FORMAT_RG32_I2F, 0, {(uint32_t)-16777219, INT32_MIN}, {0xcb800002, 0xcf000000, 0, 0x3f800000}},
        {NVN_FORMAT_RGB32_UI2F, 1, {0, 1, UINT32_MAX}, {0, 0x3f800000, 0x4f800000, 0x3f800000}},
        {NVN_FORMAT_RGB32_I2F, 2, {INT32_MAX, (uint32_t)-1, 2}, {0x4f000000, 0xbf800000, 0x40000000, 0x3f800000}},
        {NVN_FORMAT_RGBA32_UI2F, 0, {0xfffffeff, 0xffffff7f, 0xffffff80, 0xffffff81},
                                    {0x4f7fffff, 0x4f7fffff, 0x4f800000, 0x4f800000}},
        {NVN_FORMAT_RGBA32_I2F, 1, {0x3fffffdf, 0x3fffffe0, 0xc0000020, 0xc0000021},
                                   {0x4e7fffff, 0x4e800000, 0xce800000, 0xce7fffff}},
        {NVN_FORMAT_DEPTH24, 0, {3}, {0}, 1},
    };
    tex_t output = {.serial = 0x2100, .d = {.width = 1, .height = 1, .depth = 1, .levels = 1,
                    .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        list_t list = {0};
        uint8_t pixel[4] = {0};
        uint32_t check[8] = {0};
        memcpy(check, cases[c].expected, 16); check[4] = cases[c].location;
        va_st va[] = {{.fmt = NVN_FORMAT_RGBA32F}, {.fmt = NVN_FORMAT_RGBA32UI}, {.fmt = NVN_FORMAT_RGBA32I}};
        va[cases[c].location].fmt = cases[c].format;
        if (cases[c].second_format) { va[2].fmt = cases[c].second_format; check[4] = 3; }
        p_bind_vattr attr = {.n = 3}; memcpy(attr.st, va, sizeof va);
        RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &output, .r = {.width = 1, .height = 1, .depth = 1}, .mask = 15, .c.f = {0, 0, 1, 1});
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &numeric_program, .stages = 3);
        put(&list, OP_BIND_VATTR, &attr, sizeof attr);
        RECORD(OP_BIND_VSTREAM, p_bind_vstream, .n = 1, .st = {{.stride = 0}});
        RECORD(OP_BIND_VBUF1, p_bind_vbuf1, .index = 0, .size = 16, .addr = (uintptr_t)cases[c].data);
        RECORD(OP_BIND_UBO_SMALL, p_bind_ubo_small, .stage = 0, .index = 0, .size = sizeof check, .addr = (uintptr_t)check);
        RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)pixel, .r = {.width = 1, .height = 1, .depth = 1});
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes);
        assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        const uint8_t expected[] = {0, cases[c].reject ? 0 : 255, cases[c].reject ? 255 : 0, 255};
        if (memcmp(pixel, expected, 4)) {
            fprintf(stderr, "vertex format %#x to location %u: pixel %u %u %u %u\n", cases[c].format,
                    cases[c].location, pixel[0], pixel[1], pixel[2], pixel[3]);
            abort();
        }
    }
    puts("Vulkan vertex fetch values and rejected conversions passed");
}

int main(void) {
    clear_and_readback();
    textured_draw(0, 0);  // CPU argument block, followed by a fully GPU-generated block
    textured_draw(1, 0);  // X is GPU-generated, Y and Z are CPU-written
    textured_draw(0, 1);  // argument producer and consumer in successive submissions
    vertex_fetch();
    strided_layers();
    puts("NVN compute image writes, cube-sampled draw, full/compact bindings and fenced Vulkan readback passed");
}
