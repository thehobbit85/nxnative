// Submit real NVN records to the Vulkan driver, wait for completion and verify CPU-visible pixels.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include <assert.h>
#include <time.h>

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
static prog_t program = {.nstage = 2};
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) {
    assert(p == &program && stage < 2);
    memset(key, 0, 8); key[7] = (uint8_t)(stage + 1); return 1;
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
    struct { op_hdr h; p_copy_t2t p; } copy = {
        .h = {OP_COPY_T2B, sizeof copy}, .p = {.src = &tex, .dst = (const tex_t *)(void *)pixels, .sr = {.width = 8, .height = 8, .depth = 1}}};
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

static void textured_draw(int compact) {
    list_t list = {0};
    tex_t faces = {.serial = 10 + 2 * compact, .d = {.width = 8, .height = 8, .depth = 6, .levels = 1,
                   .target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8}};
    tex_t output = {.serial = faces.serial + 1, .d = {.width = 8, .height = 8, .depth = 1, .levels = 1,
                    .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&faces.d, &faces.L); nvn_tex_layout(&output.d, &output.L);
    unsigned slot = compact ? 7 : 5;
    view_t cube = {.set_target = 1, .target = NVN_TEXTURE_TARGET_CUBEMAP, .set_layers = 1, .nlayers = 6};
    mtl_reg_tex(slot, &faces, &cube);
    float positions[] = {-1,-1, 3,-1, -1,3};
    float color[] = {compact ? 1.f : 0.f, compact ? 0.f : 1.f, 1.f, 1.f};
    uint8_t pixels[8 * 8 * 4] = {0};
    RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &faces, .mask = 15, .c.f = {1, 1, 0, 1});
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
    RECORD(OP_COPY_T2B, p_copy_t2t, .src = &output, .dst = (const tex_t *)(void *)pixels, .sr = {.width = 8, .height = 8, .depth = 1});
    put(&list, OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(list.bytes);
    assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    const uint8_t expected[] = {compact ? 255 : 0, compact ? 0 : 255, 0, 255};
    for (size_t i = 0; i < sizeof pixels; i += 4) {
        if (memcmp(pixels + i, expected, 4)) {
            fprintf(stderr, "binding form %d, pixel %zu: %u %u %u %u\n", compact, i / 4, pixels[i], pixels[i+1], pixels[i+2], pixels[i+3]);
            abort();
        }
    }
}

int main(void) {
    clear_and_readback();
    textured_draw(0);
    textured_draw(1);
    puts("Textured NVN draw, cube view, full/compact bindings and fenced Vulkan readback passed");
}
