// Submit real NVN records to the Vulkan driver, wait for completion and verify CPU-visible pixels.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "../../tex/nvn_deswizzle.h"
#include <vulkan/vulkan.h>
#include <assert.h>
#include <time.h>

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
static prog_t program = {.nstage = 2}, numeric_program = {.nstage = 2}, bindless_program = {.nstage = 2}, compute = {.nstage = 1}, groups = {.nstage = 1};
static prog_t driver_program = {.nstage = 1};
static prog_t depth_copy_program = {.nstage = 2};
static prog_t output_program = {.nstage = 2};
static prog_t origin_program = {.nstage = 2};
static dpool_t texture_pool, sampler_pool, alternate_texture_pool, alternate_sampler_pool;
int nvn_dpool_base(const void *pool, int texture) {
    return texture && pool == &texture_pool ? 2048 : texture && pool == &alternate_texture_pool ? 4096 :
           !texture && pool == &sampler_pool ? 256 : !texture && pool == &alternate_sampler_pool ? 512 : 0;
}
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) {
    if (p == &origin_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x37 + stage; return 1; }
    if (p == &depth_copy_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x35 + stage; return 1; }
    if (p == &output_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x33 + stage; return 1; }
    if (p == &driver_program) { assert(stage == 0); memset(key, 0, 8); key[7] = 0x32; return 1; }
    if (p == &compute || p == &groups) { assert(stage == 0); memset(key, 0, 8); key[7] = p == &groups ? 4 : 3; return 1; }
    if (p == &bindless_program) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x31 : 1; return 1; }
    assert((p == &program || p == &numeric_program) && stage < 2);
    memset(key, 0, 8); key[7] = (uint8_t)((p == &numeric_program ? 0x21 : 1) + stage); return 1;
}
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { abort(); }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { abort(); }

static void clear_and_readback(NVNformat format) {
    tex_t tex = {.serial = 0x3000 + format, .d = {.width = 8, .height = 8, .depth = 1, .levels = 1,
                                    .target = NVN_TEXTURE_TARGET_2D, .format = format}};
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

// The payload width is in bytes. The recorder allows partial updates to either stage group's block.
static void push_constants(list_t *list, int compute, uint16_t offset, const void *data, size_t size) {
    _Alignas(p_push_const) uint8_t bytes[sizeof(p_push_const) + 16];
    assert(size <= 16);
    p_push_const *p = (p_push_const *)bytes;
    p->compute = compute; p->offset = offset; p->size = (uint32_t)size;
    memcpy(p->data, data, size);
    put(list, OP_PUSH_CONST, p, sizeof *p + size);
}

static void textured_draw(int compact, int split_submit) {
    list_t list = {0};
    tex_t faces = {.serial = 10 + 2 * compact + 100 * split_submit, .d = {.width = 8, .height = 8, .depth = 6, .levels = 1,
                   .target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8, .flags = NVN_TEXTURE_FLAGS_IMAGE}};
    tex_t output = {.serial = faces.serial + 1, .d = {.width = 8, .height = 8, .depth = 1, .levels = 1,
                    .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&faces.d, &faces.L); nvn_tex_layout(&output.d, &output.L);
    unsigned slot = compact ? 7 : 5;
    view_t cube = {.set_format = 1, .format = NVN_FORMAT_RGBA8_SRGB, .set_target = 1, .target = NVN_TEXTURE_TARGET_CUBEMAP, .set_layers = 1, .nlayers = 6};
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
    float tint[] = {1, 1, 1, 0}, alpha = 1;
    uint32_t dims[] = {8, 8, 1, 0}, depth = 6;
    push_constants(&list, 0, 240, tint, sizeof tint);
    push_constants(&list, 1, 240, dims, sizeof dims);
    push_constants(&list, 0, 252, &alpha, sizeof alpha);
    push_constants(&list, 1, 248, &depth, sizeof depth);
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

static int astc_supported(void) {
    VkInstance instance;
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "upload replay", 0, NULL, 0, VK_API_VERSION_1_1};
    info.pApplicationInfo = &app;
    assert(vkCreateInstance(&info, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, &physical);
    assert((result == VK_SUCCESS || result == VK_INCOMPLETE) && count);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    vkDestroyInstance(instance, NULL);
    return features.textureCompressionASTC_LDR;
}

// Uploads execute in a separate command buffer before the draw buffer. Queue
// ordering alone does not make their writes visible to sampling. In particular,
// SwiftShader samples zero from ASTC images without the image dependency.
static void uploaded_texture(NVNformat format) {
    tex_t texture = {.serial = 0x4000 + format, .has_layout = 1,
        .d = {.width = 4, .height = 4, .depth = 6, .levels = 1,
              .target = NVN_TEXTURE_TARGET_CUBEMAP, .format = format}};
    tex_t output = {.serial = texture.serial + 0x100, .d = {.width = 4, .height = 4,
        .depth = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&texture.d, &texture.L); nvn_tex_layout(&output.d, &output.L);
    texture.mem = calloc(1, texture.L.size); assert(texture.mem);
    mtl_reg_tex(800, &texture, NULL);
    float positions[] = {-1,-1, 3,-1, -1,3}, white[] = {1,1,1,1};
    for (int pass = 0; pass < 2; ++pass) {
        uint8_t color[] = {pass ? 255 : 0, pass ? 0 : 255, 255, 255};
        uint8_t linear[4 * 4 * 4];
        for (size_t i = 0; i < sizeof linear; i += 4) memcpy(linear + i, color, 4);
        // ASTC LDR void-extent block, followed by four UNORM16 constant channels.
        uint8_t block[16] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        for (int c = 0; c < 4; ++c) block[8 + c * 2] = block[9 + c * 2] = color[c];
        for (unsigned layer = 0; layer < 6; ++layer)
            assert(!nvn_swizzle(&texture.d, 0, layer,
                format == NVN_FORMAT_RGBA8 ? linear : block, 16, texture.mem));
        if (pass) mtl_tex_cpu_written(&texture);
        list_t list = {0};
        uint8_t pixels[4 * 4 * 4] = {0};
        push_constants(&list, 0, 240, white, sizeof white);
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &program, .stages = 3);
        va_st va = {.fmt = NVN_FORMAT_RG32F}; p_bind_vattr attr = {.n = 1};
        memcpy(attr.st, &va, sizeof va); put(&list, OP_BIND_VATTR, &attr, sizeof attr);
        RECORD(OP_BIND_VSTREAM, p_bind_vstream, .n = 1, .st = {{.stride = 8}});
        RECORD(OP_BIND_VBUF1, p_bind_vbuf1, .index = 0, .size = sizeof positions, .addr = (uintptr_t)positions);
        RECORD(OP_BIND_UBO_SMALL, p_bind_ubo_small, .stage = 1, .index = 2, .size = sizeof white, .addr = (uintptr_t)white);
        RECORD(OP_BIND_TEX1, p_bind_tex1, .stage = 1 | BIND_TEX1_NOBASE, .slot = 3, .h = 800);
        RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)pixels,
               .r = {.width = 4, .height = 4, .depth = 1});
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (size_t i = 0; i < sizeof pixels; i += 4) {
            if (memcmp(pixels + i, color, 4)) {
                fprintf(stderr, "uploaded format %#x pass %d pixel %zu: %u %u %u %u\n",
                        format, pass, i / 4, pixels[i], pixels[i+1], pixels[i+2], pixels[i+3]);
                abort();
            }
        }
    }
    mtl_reg_tex(800, NULL, NULL);
    mtl_tex_release(&texture); mtl_tex_release(&output);
    free(texture.mem);
    printf("Uploaded format %#x and changed CPU content sampled correctly\n", format);
}

static void uniform_texture_array(void) {
    tex_t textures[3], output = {.serial = 0x5100, .d = {.width = 4, .height = 4,
        .depth = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    for (int i = 0; i < 3; ++i) {
        textures[i] = (tex_t){.serial = 0x5000 + i, .d = {.width = 1, .height = 1, .depth = 1,
            .levels = 1, .target = i == 2 ? NVN_TEXTURE_TARGET_2D : NVN_TEXTURE_TARGET_3D, .format = NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&textures[i].d, &textures[i].L);
        mtl_reg_tex(2053 + i, &textures[i], NULL);
    }
    samp_par edge = {0}, border = {.ws = NVN_WRAP_MODE_CLAMP_TO_BORDER, .border = {1,1,1,1}};
    mtl_reg_samp(257, &edge); mtl_reg_samp(258, &border); mtl_reg_samp(513, &border);
    mtl_reg_tex(4101, &textures[1], NULL);
    uint64_t handles[80] = {0}, samplers[64] = {0};
    mtl_pool_add(handles, sizeof handles); mtl_pool_add(samplers, sizeof samplers);
    handles[44 + 7] = 5; samplers[16 + 7] = 1u << 20;
    float positions[] = {-1,-1, 3,-1, -1,3}, white[] = {1,1,1,1};
    const uint8_t expected[][4] = {{0,255,0,255}, {0,0,255,255}, {0,0,0,255},
                                   {255,255,255,255}, {0,0,0,255}, {0,0,0,255}, {0,0,255,255}, {255,255,255,255}};
    for (int pass = 0; pass < 8; ++pass) {
        list_t list = {0}; uint8_t pixels[64] = {0};
        struct { uint32_t index; float coordinate; uint32_t pad[2]; } parameters = {pass == 2 ? 31u : 7u, pass == 3 || pass == 7 ? 1.5f : 0.5f, {0}};
        for (int i = 0; i < 3; ++i) {
            p_clear_tex clear = {.tex = &textures[i], .r = {.width = 1, .height = 1, .depth = 1}, .mask = 15};
            clear.c.f[i == 0 ? 1 : i == 1 ? 2 : 0] = 1; clear.c.f[3] = 1;
            put(&list, OP_CLEAR_TEX, &clear, sizeof clear);
        }
        RECORD(OP_SET_TEX_POOL, p_set_pool, .pool = pass == 6 ? &alternate_texture_pool : &texture_pool);
        RECORD(OP_SET_SAMP_POOL, p_set_pool, .pool = pass == 7 ? &alternate_sampler_pool : &sampler_pool);
        struct { uint64_t address, size, value; } update = {(uintptr_t)&handles[44 + 7], 8, pass >= 6 ? 5u : pass >= 4 ? 7u : pass ? 6u : 5u};
        put(&list, OP_UPDATE_UB, &update, sizeof update);
        samplers[16 + 7] = (uint64_t)(pass == 3 ? 2 : 1) << 20;
        push_constants(&list, 0, 240, white, sizeof white);
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &bindless_program, .stages = 3);
        va_st va = {.fmt = NVN_FORMAT_RG32F}; p_bind_vattr attr = {.n = 1};
        memcpy(attr.st, &va, sizeof va); put(&list, OP_BIND_VATTR, &attr, sizeof attr);
        RECORD(OP_BIND_VSTREAM, p_bind_vstream, .n = 1, .st = {{.stride = 8}});
        RECORD(OP_BIND_VBUF1, p_bind_vbuf1, .index = 0, .size = sizeof positions, .addr = (uintptr_t)positions);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 1, .index = 7, .addr = (uintptr_t)handles,
               .size = pass == 5 ? 352 : sizeof handles);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 1, .index = 8, .addr = (uintptr_t)samplers, .size = sizeof samplers);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 1, .index = 9, .addr = (uintptr_t)&parameters, .size = sizeof parameters);
        RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)pixels, .r = {.width = 4, .height = 4, .depth = 1});
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (size_t i = 0; i < sizeof pixels; i += 4) if (memcmp(pixels + i, expected[pass], 4)) {
            fprintf(stderr, "uniform texture array pass %d pixel %zu: %u %u %u %u\n", pass, i / 4, pixels[i],pixels[i+1],pixels[i+2],pixels[i+3]);
            abort();
        }
    }
    mtl_pool_del(handles); mtl_pool_del(samplers);
    mtl_reg_tex(4101, NULL, NULL);
    for (int i = 0; i < 3; ++i) { mtl_reg_tex(2053 + i, NULL, NULL); mtl_tex_release(&textures[i]); }
    mtl_tex_release(&output);
    puts("Uniform texture arrays, pool bases, inline updates, independent samplers and missing handles passed");
}

static void vertex_output_defaults(void) {
    list_t list = {0};
    uint8_t pixel[4] = {0};
    tex_t output = {.serial = 0x3300, .d = {.width = 1, .height = 1, .depth = 1, .levels = 1,
                    .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &output, .r = {.width=1, .height=1, .depth=1}, .mask=15, .c.f={0,0,1,1});
    RECORD(OP_SET_RT, p_set_rt, .n=1, .color={&output});
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&output_program, .stages=3);
    RECORD(OP_BIND_VATTR, p_bind_vattr, .n=0);
    RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .count=3, .instances=1);
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&output, .src=(uintptr_t)pixel, .r={.width=1, .height=1, .depth=1});
    put(&list, OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    const uint8_t green[] = {0,255,0,255};
    if (memcmp(pixel, green, 4)) {
        fprintf(stderr, "vertex output defaults: pixel %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]); abort();
    }
    puts("Unwritten vertex output components default to (0,0,0,1); explicit writes override them");
}

static void compute_driver_constants(void) {
    for (int mode = 0; mode < 5; ++mode) {
        uint32_t output[16] = {0}, uniform[16] = {0}, counts[] = {mode >= 2 ? 0u : 2u, mode == 2 ? 0u : 3u, mode == 2 ? 0u : 4u};
        list_t list = {0};
        if (mode >= 2) {
            uint32_t sizes[] = {2, 3, 4, 0};
            push_constants(&list, 1, 240, sizes, sizeof sizes);
            RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &groups, .stages = 0x20);
            RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)counts, .size = mode == 2 ? 12 : 4);
            RECORD(OP_DISPATCH, p_dispatch, .x = mode == 2 ? 3 : 1, .y = 1, .z = 1);
            if (mode == 4) {
                put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); mtl_sub_commit(); list.length = 0;
            }
        }
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &driver_program, .stages = 0x20);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 5, .index = 7, .addr = (uintptr_t)uniform, .size = sizeof uniform);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)output, .size = sizeof output);
        RECORD(OP_DISPATCH, p_dispatch, .x = 2, .y = 3, .z = 4, .ind = mode ? (uintptr_t)counts : 0);
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        uint32_t expected[] = {2,3,4,0, 2,3,4,0,
            (uint32_t)(uintptr_t)uniform, (uint32_t)((uint64_t)(uintptr_t)uniform >> 32), sizeof uniform, 0,
            (uint32_t)(uintptr_t)output, (uint32_t)((uint64_t)(uintptr_t)output >> 32), sizeof output, 0};
        for (int i = 0; i < 16; ++i) if (output[i] != expected[i]) {
            fprintf(stderr, "compute driver mode %d word %d: %#x expected %#x\n", mode, i, output[i], expected[i]); abort();
        }
    }
    puts("Compute driver dimensions and buffer tables match direct, CPU and GPU indirect dispatches");
}

// First create and clear the backing, then register image views of its layers and
// mips. A null registration must stop the dispatch; replacing it must reuse that backing.
static void registered_images(void) {
    enum { a = 600, b = 601 };
    tex_t tex = {.serial = 0x2000, .d = {.width = 4, .height = 4, .depth = 2, .levels = 2,
                 .target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&tex.d, &tex.L);
    list_t list = {0};
    view_t mip = {.set_levels = 1, .base_level = 1, .nlevels = 1};
    RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &tex, .r = {.width = 4, .height = 4, .depth = 2}, .mask = 15, .c.f = {0, 0, 1, 1});
    RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &tex, .view = mip, .has_view = 1,
           .r = {.width = 2, .height = 2, .depth = 2}, .mask = 15, .c.f = {1, 0, 1, 1});
    put(&list, OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(list.bytes); mtl_sub_commit();
    for (int pass = 0; pass < 3; pass++) {
        view_t yellow = {.set_layers = 1, .min_layer = pass == 2, .nlayers = 1,
                         .set_levels = 1, .base_level = pass == 2, .nlevels = 1};
        view_t cyan = {.set_layers = 1, .min_layer = !pass, .nlayers = 1,
                       .set_levels = 1, .base_level = pass != 0, .nlevels = 1};
        mtl_reg_image(a, pass == 1 ? NULL : &tex, &yellow);
        mtl_reg_image(b, &tex, &cyan);
        assert(!tex.d.flags);
        uint8_t pixels[2][2][16][4] = {0};
        list = (list_t){0};
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &compute, .stages = 0x20);
        RECORD(OP_BIND_IMG, p_bind_handles, .stage = 5, .first = 1, .n = 2, .h = {a, b});
        RECORD(OP_DISPATCH, p_dispatch, .x = 1, .y = 1, .z = 1);
        for (int level = 0; level < 2; ++level) for (int layer = 0; layer < 2; ++layer) {
            int w = 4 >> level;
            RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &tex, .src = (uintptr_t)pixels[level][layer],
                   .has_view = 1, .view = {.set_levels = 1, .base_level = level, .nlevels = 1},
                   .r = {.zoffset = layer, .width = w, .height = w, .depth = 1});
        }
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int level = 0; level < 2; ++level) for (int layer = 0; layer < 2; ++layer) {
            int w = 4 >> level;
            for (int i = 0; i < w * w; ++i) {
                int written = !i && (!level || pass == 2), yellow_pixel = layer == level;
                uint8_t expected[] = {written ? (yellow_pixel ? 255 : 0) : (level ? 255 : 0),
                                      written ? 255 : 0, written && yellow_pixel ? 0 : 255, 255};
                assert(!memcmp(pixels[level][layer][i], expected, 4));
            }
        }
    }
    puts("Late image registration, descriptor replacement and all untouched pixels passed");
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

// Two queues retain independent render targets and copy strides across submissions.
// Finalizing a program must also clear bindings saved on an inactive queue.
static void queue_lifetime(void) {
    tex_t target[2] = {0};
    uint8_t pixels[2][80];
    for (int q = 0; q < 2; q++) {
        target[q] = (tex_t){.serial = 0x8000 + q, .d = {.width = 4, .height = 4, .depth = 1,
            .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&target[q].d, &target[q].L);
        list_t list = {0};
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&target[q]});
        RECORD(OP_COPY_STRIDE, p_copy_stride, .stride = q ? 20 : 16);
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &program, .stages = 3);
        put(&list, OP_END, NULL, 0);
        mtl_queue_activate(100 + q); mtl_sub_begin(); mtl_exec(list.bytes);
        assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    }
    mtl_prog_forget(&program);
    for (int q = 0; q < 2; q++) {
        list_t list = {0};
        memset(pixels[q], 0xcc, sizeof pixels[q]);
        RECORD(OP_CLEAR_COLOR, p_clear_color, .mask = 15, .rgba = {q ? 0 : 1, q ? 1 : 0, 0, 1});
        // With the finalized program unbound this draw is skipped, leaving the clear.
        RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &target[q], .src = (uintptr_t)pixels[q],
               .r = {.width = 4, .height = 4, .depth = 1});
        put(&list, OP_END, NULL, 0);
        mtl_queue_activate(100 + q); mtl_sub_begin(); mtl_exec(list.bytes);
        assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            const uint8_t expected[] = {q ? 0 : 255, q ? 255 : 0, 0, 255};
            assert(!memcmp(pixels[q] + y * (q ? 20 : 16) + x * 4, expected, 4));
        }
        if (q) for (int y = 0; y < 4; y++) for (int x = 16; x < 20; x++) assert(pixels[q][y * 20 + x] == 0xcc);
    }
    mtl_queue_forget(100); mtl_queue_forget(101);
    puts("Vulkan queue targets, copy strides and finalized program bindings passed");
}

// Rectangles address memory rows regardless of device or queue origin.
static void queue_viewport_origin(void) {
    tex_t output = {.serial = 0x8100, .d = {.width = 4, .height = 4, .depth = 1, .levels = 1,
                    .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    for (int device = 0; device < 2; device++) for (int queue = 0; queue < 2; queue++) {
        uint32_t zero[8] = {0}; uint8_t pixels[64] = {0}; list_t list = {0};
        va_st va[] = {{.fmt = NVN_FORMAT_RGBA32F}, {.fmt = NVN_FORMAT_RGBA32UI}, {.fmt = NVN_FORMAT_RGBA32I}};
        p_bind_vattr attr = {.n = 3}; memcpy(attr.st, va, sizeof va);
        RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &output, .r = {.width = 4, .height = 4, .depth = 1}, .mask = 15, .c.f = {0, 0, 1, 1});
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &numeric_program, .stages = 3);
        put(&list, OP_BIND_VATTR, &attr, sizeof attr);
        RECORD(OP_BIND_VSTREAM, p_bind_vstream, .n = 1, .st = {{.stride = 0}});
        RECORD(OP_BIND_VBUF1, p_bind_vbuf1, .index = 0, .size = 16, .addr = (uintptr_t)zero);
        RECORD(OP_BIND_UBO_SMALL, p_bind_ubo_small, .stage = 0, .index = 0, .size = sizeof zero, .addr = (uintptr_t)zero);
        RECORD(OP_VIEWPORTS, p_viewports, .n = 1, .v = {{0, 0, 4, 2}});
        RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)pixels, .r = {.width = 4, .height = 4, .depth = 1});
        put(&list, OP_END, NULL, 0);
        mtl_set_origin_lower_left(device); mtl_queue_activate(110 + queue); mtl_set_queue_origin_lower_left(queue);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            // Viewport coordinates name memory rows in either convention.
            int drawn = y < 2;
            const uint8_t expected[] = {0, drawn ? 255 : 0, drawn ? 0 : 255, 255};
            assert(!memcmp(pixels + (y * 4 + x) * 4, expected, 4));
        }
    }
    mtl_queue_forget(110); mtl_queue_forget(111);
    puts("Vulkan viewport rectangles address the same memory rows for both origins");
}

// Same expectations as Metal's t_queue_origin: queue-latched raster origin
// changes facing; the device origin changes clip Y within the viewport only.
static void origin_facing(void) {
    tex_t output = {.serial=0x8200, .d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    const int front_in_clip[2][2][2] = {{{0,1},{1,0}},{{1,0},{0,1}}};
    for (int device=0;device<2;device++) for (int queue=0;queue<2;queue++)
    for (int winding=0;winding<2;winding++) for (int cull=0;cull<4;cull++) for (int ccw=0;ccw<2;ccw++) {
        float positions[3][2]={{-1,-1},{3,-1},{-1,3}};
        if (!ccw) { float tmp[2]; memcpy(tmp,positions[1],8); memcpy(positions[1],positions[2],8); memcpy(positions[2],tmp,8); }
        uint8_t pixels[256]={0}; list_t list={0};
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&output,.r={.width=8,.height=8,.depth=1},.mask=15,.c.f={0,0,1,1});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&origin_program,.stages=3);
        p_bind_state bs={.kind=4}; poly_st poly={.front=winding,.cull=cull}; memcpy(bs.bytes,&poly,sizeof poly); put(&list,OP_BIND_STATE,&bs,sizeof bs);
        va_st va={.fmt=NVN_FORMAT_RG32F}; p_bind_vattr attr={.n=1}; memcpy(attr.st,&va,sizeof va); put(&list,OP_BIND_VATTR,&attr,sizeof attr);
        RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=1,.st={{.stride=8}});
        RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=sizeof positions,.addr=(uintptr_t)positions);
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,8,8}});
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
        put(&list,OP_END,NULL,0);
        mtl_set_origin_lower_left(device); mtl_queue_activate(120+queue); mtl_set_queue_origin_lower_left(queue);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        int front=ccw==front_in_clip[queue][device][winding];
        int visible=cull==NVN_FACE_NONE || (cull==NVN_FACE_FRONT && !front) || (cull==NVN_FACE_BACK && front);
        uint8_t want[]={visible&&!front?255:0,visible&&front?255:0,visible?0:255,255};
        for (int i=0;i<64;i++) { if(memcmp(pixels+4*i,want,4)) fprintf(stderr,"origin device%d queue%d winding%d cull%d ccw%d got%d,%d,%d expected%d,%d,%d\n",device,queue,winding,cull,ccw,pixels[4*i],pixels[4*i+1],pixels[4*i+2],want[0],want[1],want[2]); assert(!memcmp(pixels+4*i,want,4)); }
    }
    for (int device=0;device<2;device++) {
        // Bottom half of clip space, offset viewport, then an unflipped scissor.
        float positions[][2]={{-1,-1},{1,-1},{-1,0},{-1,0},{1,-1},{1,0}};
        uint8_t pixels[256]={0}; list_t list={0};
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&output,.r={.width=8,.height=8,.depth=1},.mask=15,.c.f={0,0,1,1});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&origin_program,.stages=3);
        p_bind_state bs={.kind=4}; poly_st poly={.front=NVN_FRONT_FACE_CCW}; memcpy(bs.bytes,&poly,sizeof poly); put(&list,OP_BIND_STATE,&bs,sizeof bs);
        va_st va={.fmt=NVN_FORMAT_RG32F}; p_bind_vattr attr={.n=1}; memcpy(attr.st,&va,sizeof va); put(&list,OP_BIND_VATTR,&attr,sizeof attr);
        RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=1,.st={{.stride=8}});
        RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=sizeof positions,.addr=(uintptr_t)positions);
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,1,8,6}});
        RECORD(OP_SCISSOR,p_scissor,.x=1,.y=2,.w=6,.h=3);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=6,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
        put(&list,OP_END,NULL,0);
        mtl_set_origin_lower_left(device); mtl_queue_activate(122+device); mtl_set_queue_origin_lower_left(device);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(int y=0;y<8;y++) for(int x=0;x<8;x++) {
            int drawn=x>=1 && x<7 && (device ? (y==2 || y==3) : y==4);
            uint8_t want[]={0,drawn?255:0,drawn?0:255,255}; assert(!memcmp(pixels+4*(8*y+x),want,4));
        }
        mtl_queue_forget(122+device);
    }
    mtl_queue_forget(120); mtl_queue_forget(121); mtl_set_origin_lower_left(0);
    puts("Vulkan queue/device origins preserve facing and every cull mode (128 cases)");
}

static void depth_copies(void) {
    const NVNformat formats[] = {NVN_FORMAT_DEPTH32F, NVN_FORMAT_DEPTH16, NVN_FORMAT_DEPTH24_STENCIL8, NVN_FORMAT_DEPTH32F_STENCIL8};
    for (unsigned k=0;k<sizeof formats/sizeof *formats;k++) for (int scale=0;scale<2;scale++) {
        tex_t source = {.serial=0x9000+32*k+4*scale, .d={.width=16,.height=16,.depth=2,.levels=2,.target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=formats[k]}};
        tex_t target=source; target.serial++;
        tex_t output = {.serial=source.serial+2,.d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&source.d,&source.L); nvn_tex_layout(&target.d,&target.L); nvn_tex_layout(&output.d,&output.L);
        view_t slice={.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=1,.nlayers=1,.set_target=1,.target=NVN_TEXTURE_TARGET_2D};
        mtl_reg_tex(900,&target,&slice);
        samp_par nearest={0}; mtl_reg_samp(900,&nearest);
        uint8_t pixels[256]={0}; list_t list={0}; int sw=scale?4:2;
        p_clear_tex clear={.has_view=1,.view=slice,.r={.width=8,.height=8,.depth=1},.mask=15};
        clear.tex=&source; clear.c.f[0]=.25f; clear.c.u[1]=7; put(&list,OP_CLEAR_TEX,&clear,sizeof clear);
        clear.tex=&target; clear.c.f[0]=.75f; clear.c.u[1]=0; put(&list,OP_CLEAR_TEX,&clear,sizeof clear);
        RECORD(OP_COPY_T2T,p_copy_t2t,.src=&source,.dst=&target,.has_sv=1,.sv=slice,.has_dv=1,.dv=slice,
               .sr={.xoffset=2,.yoffset=2,.width=sw,.height=sw,.depth=1},.dr={.xoffset=1,.yoffset=1,.width=2,.height=2,.depth=1});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_copy_program,.stages=3);
        RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1|BIND_TEX1_NOBASE,.slot=0,.h=900 | (900ull<<20));
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,8,8}});
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
        put(&list,OP_END,NULL,0);
        mtl_queue_activate(901); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(int y=0;y<8;y++) for(int x=0;x<8;x++) {
            int copied=x>=1&&x<3&&y>=1&&y<3, want=copied?64:191;
            if(abs(pixels[4*(y*8+x)]-want)>1) {
                fprintf(stderr,"depth copy format %#x scale%d at %d,%d got%u wanted%d\n",formats[k],scale,x,y,pixels[4*(y*8+x)],want); abort();
            }
        }
        if(formats[k]==NVN_FORMAT_DEPTH24_STENCIL8 || formats[k]==NVN_FORMAT_DEPTH32F_STENCIL8) {
            list=(list_t){0}; uint32_t zero[8]={0};
            va_st va[]={{.fmt=NVN_FORMAT_RGBA32F},{.fmt=NVN_FORMAT_RGBA32UI},{.fmt=NVN_FORMAT_RGBA32I}};
            p_bind_vattr attr={.n=3}; memcpy(attr.st,va,sizeof va);
            p_bind_state depth={.kind=5}; ds_st ds={.st=1,.sf_f=NVN_COMPARE_FUNC_EQUAL,.sf_b=NVN_COMPARE_FUNC_EQUAL,
                .fail_f=NVN_STENCIL_OP_KEEP,.zfail_f=NVN_STENCIL_OP_KEEP,.zpass_f=NVN_STENCIL_OP_KEEP,
                .fail_b=NVN_STENCIL_OP_KEEP,.zfail_b=NVN_STENCIL_OP_KEEP,.zpass_b=NVN_STENCIL_OP_KEEP}; memcpy(depth.bytes,&ds,sizeof ds);
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output},.depth=&target,.has_dview=1,.dview=slice);
            RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=15,.rgba={0,0,1,1});
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&numeric_program,.stages=3);
            put(&list,OP_BIND_STATE,&depth,sizeof depth); put(&list,OP_BIND_VATTR,&attr,sizeof attr);
            RECORD(OP_STENCIL,p_stencil,.face=3,.which=2,.ref=7);
            RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=1,.st={{.stride=0}});
            RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=16,.addr=(uintptr_t)zero);
            RECORD(OP_BIND_UBO_SMALL,p_bind_ubo_small,.stage=0,.index=0,.size=sizeof zero,.addr=(uintptr_t)zero);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
            put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for(int y=0;y<8;y++) for(int x=0;x<8;x++) {
                int copied=x>=1&&x<3&&y>=1&&y<3;
                assert(pixels[4*(y*8+x)+1]==(copied?255:0)); assert(pixels[4*(y*8+x)+2]==(copied?0:255));
            }
        }
        mtl_queue_forget(901); mtl_reg_tex(900,NULL,NULL); mtl_tex_release(&source);mtl_tex_release(&target);mtl_tex_release(&output);
    }
    puts("Depth and stencil copies preserve subrectangles, mip levels and array layers at equal and scaled sizes");
}

int main(void) {
    depth_copies();
    clear_and_readback(NVN_FORMAT_RGBA8);
    clear_and_readback(NVN_FORMAT_RGBA8_SRGB);
    uploaded_texture(NVN_FORMAT_RGBA8);
    if (astc_supported()) uploaded_texture(NVN_FORMAT_RGBA_ASTC_4x4);
    else puts("ASTC upload sampling skipped: device lacks textureCompressionASTC_LDR");
    uniform_texture_array();
    compute_driver_constants();
    vertex_output_defaults();
    textured_draw(0, 0);  // CPU argument block, followed by a fully GPU-generated block
    textured_draw(1, 0);  // X is GPU-generated, Y and Z are CPU-written
    textured_draw(0, 1);  // argument producer and consumer in successive submissions
    vertex_fetch();
    registered_images();
    strided_layers();
    queue_lifetime();
    origin_facing();
    queue_viewport_origin();
    puts("NVN compute image writes, cube-sampled draw, full/compact bindings and fenced Vulkan readback passed");
}
