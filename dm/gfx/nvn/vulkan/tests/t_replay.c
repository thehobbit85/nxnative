// Submit real NVN records to the Vulkan driver, wait for completion and verify CPU-visible pixels.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_timestamp.h"
#include "../../tex/nvn_deswizzle.h"
#include <vulkan/vulkan.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
static int test_nvn2;
int dm_sdk_ounce(void) { return test_nvn2; }
static prog_t program = {.nstage = 2}, numeric_program = {.nstage = 2}, bindless_program = {.nstage = 2}, compute = {.nstage = 1}, groups = {.nstage = 1};
static prog_t packed_program={.nstage=2};
static prog_t capture_program={.nstage=1};
static prog_t restart_program={.nstage=2};
static prog_t table_program[2] = {{.nstage=2},{.nstage=2}};
static prog_t warp_program[2] = {{.nstage = 1}, {.nstage = 1}};
static prog_t driver_program = {.nstage = 1};
static prog_t typed_border_program[] = {{.nstage=1},{.nstage=1}};
static prog_t resource_shapes_program = {.nstage = 1};
static prog_t driver_c2_program = {.nstage = 1};
static prog_t c2_sample_program = {.nstage = 2};
static prog_t alias_program = {.nstage = 1};
static prog_t texel_program = {.nstage = 1};
static prog_t texel_graphics_program = {.nstage = 2};
static prog_t separate_program[4]={{.nstage=2},{.nstage=2},{.nstage=2},{.nstage=2}};
static prog_t discard_image_program = {.nstage=2};
static prog_t storage_program = {.nstage = 2};
static prog_t uniform_storage_program = {.nstage = 2};
static prog_t split_program[3] = {{.nstage = 2}, {.nstage = 2}, {.nstage = 2}};
static prog_t buffer_program = {.nstage = 1};
static prog_t depth_copy_program = {.nstage = 2};
static prog_t image_draw = {.nstage = 2}, image_compute = {.nstage = 1};
static prog_t feedback_program = {.nstage = 2};
static prog_t depth_clamp_program = {.nstage = 2};
static prog_t offset_program = {.nstage=2}, fill_program = {.nstage=2};
static prog_t alpha_program[3] = {{.nstage=2},{.nstage=2},{.nstage=2}};
static prog_t mixed_color_program={.nstage=2};
static prog_t logic_program[2] = {{.nstage=2},{.nstage=2}};
static prog_t depth_range_program = {.nstage = 2};
static prog_t one_sample_program = {.nstage = 2};
static prog_t blend_mrt_program = {.nstage = 2}, sparse_mrt_program = {.nstage = 2};
static prog_t divisor_program = {.nstage = 2};
static prog_t output_program = {.nstage = 2};
static prog_t origin_program = {.nstage = 2};
static prog_t draw_parameters_program = {.nstage = 2};
static dpool_t texture_pool, sampler_pool, alternate_texture_pool, alternate_sampler_pool;
static const void *flushed_storage;
// The raw replay fixture has no shared recorder or sparse guest page tables.
int nvn_ft_tracked(uint64_t serial) { (void)serial; return 0; }
int nvn_sparse_pool(const void *pool) { (void)pool; return 0; }
uint64_t nvn_sparse_gen(void) { abort(); } // Sparse paths are tested with the real public recorder.
int nvn_sparse_since(const void *p, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp) { abort(); }
void nvn_sparse_protect(const void *p, uint64_t a, uint64_t n) { abort(); }
uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t addr, uint64_t size) { abort(); }
int nvn_flushed(const void *p, uint64_t n) { (void)n; return p == flushed_storage; }
int nvn_dpool_base(const void *pool, int texture) {
    return texture && pool == &texture_pool ? 2048 : texture && pool == &alternate_texture_pool ? 4096 :
           !texture && pool == &sampler_pool ? 256 : !texture && pool == &alternate_sampler_pool ? 512 : 0;
}
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) {
    for (int i=0;i<2;++i) if (p==&typed_border_program[i]) { assert(stage==0); memset(key,0,8); key[7]=0x69+i; return 1; }
    if (p==&capture_program) { assert(stage==0); const uint8_t k[8]={0xde,0x21,0x67,0x83,0xc4,0xf7,0x06,0x05}; memcpy(key,k,8); return 1; }
    if (p==&mixed_color_program) { assert(stage<2); memset(key,0,8); key[7]=stage ? 0x67 : 0x52; return 1; }
    for (int i=0;i<2;++i) if (p==&logic_program[i]) { assert(stage<2); memset(key,0,8); key[7]=stage ? 0x64+i : 0x52; return 1; }
    for (int i=0;i<3;++i) if (p==&alpha_program[i]) { assert(stage<2); memset(key,0,8); key[7]=stage ? 0x61+i : 0x52; return 1; }
    if (p == &one_sample_program) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x54 : 0x52; return 1; }
    if (p == &sparse_mrt_program) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x66 : 0x52; return 1; }
    if (p == &blend_mrt_program) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x55 : 0x52; return 1; }
    if (p == &divisor_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x56+stage; return 1; }
    if (p == &image_draw || p == &image_compute) { assert(stage < p->nstage); memset(key, 0, 8); key[7] = p == &image_compute ? 0x5f : stage ? 0x5e : 0x35; return 1; }
    if (p == &feedback_program) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x5d : 0x35; return 1; }
    if (p == &fill_program) { assert(stage < 2); memset(key,0,8); key[7] = stage ? 0x53 : 0x68; return 1; }
    if (p == &offset_program) { assert(stage < 2); memset(key,0,8); key[7] = stage ? 0x53 : 0x60; return 1; }
    if (p == &depth_clamp_program) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x53 : 0x5c; return 1; }
    if (p == &depth_range_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x52 + stage; return 1; }
    for (int i = 0; i < 2; i++) if (p == &warp_program[i]) { assert(stage == 0); memset(key, 0, 8); key[7] = 0x52 + i; return 1; }
    if (p == &resource_shapes_program) { assert(stage == 0); const uint8_t k[8]={0,0,0,0,0xfe,0xed,0x61,0};memcpy(key,k,8);return 1; }
    if (p == &c2_sample_program) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x51 : 0x35; return 1; }
    if (p == &driver_c2_program) { assert(stage == 0); memset(key, 0, 8); key[7] = 0x50; return 1; }
    for (int i = 0; i < 3; i++) if (p == &split_program[i]) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x4d+i : 0x35; return 1; }
    if (p == &uniform_storage_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x4b + stage; return 1; }
    if (p == &texel_graphics_program) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x49 : 0x4a; return 1; }
    if (p == &texel_program) { assert(stage == 0); memset(key, 0, 8); key[7] = 0x48; return 1; }
    for(int i=0;i<4;i++) if(p==&separate_program[i]) { assert(stage<2);memset(key,0,8);key[7]=stage?0x44+i:1;return 1; }
    if (p == &alias_program) { assert(stage == 0); memset(key, 0, 8); key[7] = 0x43; return 1; }
    if (p == &discard_image_program) { assert(stage<2); memset(key,0,8); key[7]=stage ? 0x42 : 0x6c; return 1; }
    if (p == &storage_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x41 + stage; return 1; }
    if (p == &buffer_program) { assert(stage == 0); memset(key, 0, 8); key[7] = 0x40; return 1; }
    if (p == &draw_parameters_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x39 + stage; return 1; }
    if (p == &origin_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x37 + stage; return 1; }
    if (p == &depth_copy_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x35 + stage; return 1; }
    if (p == &output_program) { assert(stage < 2); memset(key, 0, 8); key[7] = 0x33 + stage; return 1; }
    if (p == &driver_program) { assert(stage == 0); memset(key, 0, 8); key[7] = 0x32; return 1; }
    if (p == &compute || p == &groups) { assert(stage == 0); memset(key, 0, 8); key[7] = p == &groups ? 4 : 3; return 1; }
    if (p == &packed_program) { assert(stage < 2); memset(key,0,8); key[7]=stage ? 0x22 : 0x5b; return 1; }
    if (p == &restart_program) { assert(stage < 2); memset(key,0,8); key[7]=stage ? 0x38 : 0x5a; return 1; }
    if (p == &table_program[0] || p == &table_program[1]) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x58 + (p == &table_program[1]) : 1; return 1; }
    if (p == &bindless_program) { assert(stage < 2); memset(key, 0, 8); key[7] = stage ? 0x31 : 1; return 1; }
    assert((p == &program || p == &numeric_program) && stage < 2);
    memset(key, 0, 8); key[7] = (uint8_t)((p == &numeric_program ? 0x21 : 1) + stage); return 1;
}
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { abort(); }
void dm_guest_write_prepare(void *addr, size_t len) { (void)addr; (void)len; }  // raw fixture storage uses writable host memory
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
    assert(!mtl_tex_gpu_owned(tex.serial));
    mtl_sub_begin();
    mtl_exec((const uint8_t *)&list);
    uint64_t fence = mtl_sub_commit();
    assert(mtl_wait(fence, UINT64_MAX));
    assert(mtl_tex_gpu_owned(tex.serial));
    for (size_t i = 0; i < sizeof pixels; i += 4)
        assert(pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 255 && pixels[i + 3] == 255);
    puts("NVN clear, texture readback and Vulkan fence returned 64 expected pixels");
    mtl_tex_release(&tex);
    assert(!mtl_tex_gpu_owned(tex.serial));
}

#include "../../../tests/recorded_list.h"
#include "../../../tests/polygon_offset_corpus.h"
#include "../../../tests/polygon_fill_corpus.h"
#include "../../../tests/alpha_coverage_corpus.h"

// The payload width is in bytes. The recorder allows partial updates to either stage group's block.
static void push_constants(list_t *list, int compute, uint16_t offset, const void *data, size_t size) {
    _Alignas(p_push_const) uint8_t bytes[sizeof(p_push_const) + 16] = {0};
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
    uint64_t deferred_handle=0;
    if(!compact) {
        uint32_t handle_words[4]={slot,0,0,0};
        push_constants(&list,1,240,handle_words,sizeof handle_words);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)&deferred_handle,.size=8);
        RECORD(OP_DISPATCH,p_dispatch,.x=2,.y=1,.z=1);
    }
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
        RECORD(OP_BIND_TEX, p_bind_handles, .stage = 1, .first = 3, .n = 1, .defer = 1, .src = (uintptr_t)&deferred_handle);
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

static VkPhysicalDeviceFeatures replay_features(int *custom_border, uint32_t *timestamp_bits) {
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
    if (timestamp_bits) {
        VkQueueFamilyProperties families[16]; uint32_t count=16;
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
        *timestamp_bits=0;
        for (uint32_t i=0;i<count;i++) if (families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) {
            if (!getenv("DM_VK_NO_GPU_TIMESTAMPS")) *timestamp_bits=families[i].timestampValidBits;
            break;
        }
    }
    if (custom_border) {
        uint32_t n=0;
        assert(vkEnumerateDeviceExtensionProperties(physical,NULL,&n,NULL)==VK_SUCCESS);
        VkExtensionProperties *ext=calloc(n?n:1,sizeof *ext);
        assert(ext && vkEnumerateDeviceExtensionProperties(physical,NULL,&n,ext)==VK_SUCCESS);
        *custom_border=0;
        for (uint32_t i=0; i<n; ++i) if (!strcmp(ext[i].extensionName,VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)) {
            VkPhysicalDeviceCustomBorderColorFeaturesEXT b={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 f={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&b};
            vkGetPhysicalDeviceFeatures2(physical,&f);
            if (b.customBorderColors && !getenv("DM_VK_NO_CUSTOM_BORDER"))
                *custom_border=1 | (b.customBorderColorWithoutFormat && !getenv("DM_VK_NO_FORMATLESS_BORDER") ? 2 : 0);
        }
        free(ext);
    }
    vkDestroyInstance(instance, NULL);
    return features;
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

// Initial-content rules reused from Metal. A CPU_UNCACHED linear R8 atlas
// needs neither a packaged layout nor a flush (Link's Awakening's font atlas).
static void initial_texture_content(void) {
    const struct { unsigned pool, flags; int flush, filled; } cases[] = {
        {NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED,0,0,1},
        {NVN_MEMORY_POOL_FLAGS_CPU_CACHED,0,1,1},
        {NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS,0,0,1},
        {NVN_MEMORY_POOL_FLAGS_CPU_CACHED,0,0,0},
        {NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED|NVN_MEMORY_POOL_FLAGS_VIRTUAL,0,0,0},
        {NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS,NVN_TEXTURE_FLAGS_COMPRESSIBLE,0,0},
        {NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED,NVN_TEXTURE_FLAGS_COMPRESSIBLE,0,1},
    };
    for (unsigned c=0;c<sizeof cases/sizeof *cases;c++) {
        uint8_t storage[64*8]; memset(storage,153,sizeof storage);
        pool_t pool={.flags=cases[c].pool,.mem=storage,.size=sizeof storage};
        tex_t source={.serial=0xa000+3*c,.mem=storage,.pool=&pool,.d={.width=64,.height=8,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R8,.flags=NVN_TEXTURE_FLAGS_IMAGE|NVN_TEXTURE_FLAGS_LINEAR|cases[c].flags,.stride=64}};
        tex_t output={.serial=source.serial+1,.d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&source.d,&source.L); nvn_tex_layout(&output.d,&output.L);
        flushed_storage=cases[c].flush?storage:NULL;
        samp_par nearest={0}; mtl_reg_samp(910,&nearest); mtl_reg_tex(910,&source,NULL);
        mtl_queue_activate(910);
        for (int pass=0;pass<2;pass++) {
            uint8_t pixels[256]={0}; list_t list={0};
            if (pass) { // Once the GPU writes it, later CPU bytes cannot replace that result.
                memset(storage,17,sizeof storage); mtl_tex_cpu_written(&source);
                RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&source,.r={.width=64,.height=8,.depth=1},.mask=15,.c.f={1,0,0,1});
            }
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_copy_program,.stages=3);
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1|BIND_TEX1_NOBASE,.slot=0,.h=910|(910ull<<20));
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
            put(&list,OP_END,NULL,0);
            mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            uint8_t want[]={pass?255:cases[c].filled?153:0,0,0,255};
            for (int i=0;i<64;i++) { if(memcmp(pixels+4*i,want,4)) fprintf(stderr,"initial content case%u pass%d: %u expected%u\n",c,pass,pixels[4*i],want[0]); assert(!memcmp(pixels+4*i,want,4)); }
        }
        mtl_reg_tex(910,NULL,NULL); mtl_tex_release(&source); mtl_tex_release(&output); mtl_queue_forget(910);
    }
    flushed_storage=NULL;
    puts("CPU-uncached, flushed and inaccessible texture initial content and GPU ownership passed");
}

enum { STOCK_BORDER, CUSTOM_BORDER, STOCK_FALLBACK };
static void uniform_texture_array(int table, int custom_border) {
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
    if (custom_border) {
        memcpy(border.border, (float[]){96.f/255,32.f/255,159.f/255,127.f/255}, sizeof border.border);
        border.minf=NVN_MIN_FILTER_LINEAR; border.magf=NVN_MAG_FILTER_LINEAR;
    }
    mtl_reg_samp(257, &edge); mtl_reg_samp(258, &border); mtl_reg_samp(513, &border);
    mtl_reg_tex(4101, &textures[1], NULL);
    uint64_t handles[8192] = {0}, samplers[8192] = {0};
    unsigned index = table ? 8057 : 7;
    mtl_pool_add(handles, sizeof handles); mtl_pool_add(samplers, sizeof samplers);
    handles[44 + index] = 5; samplers[16 + index] = 1u << 20;
    float positions[] = {-1,-1, 3,-1, -1,3}, white[] = {1,1,1,1};
    uint8_t expected[][4] = {{0,255,0,255}, {0,0,255,255}, {0,0,0,255},
                                   {255,255,255,255}, {0,0,0,255}, {0,0,0,255}, {0,0,255,255}, {255,255,255,255}};
    if (custom_border) {
        // Slot 258 samples halfway between blue and the guest border.
        if (custom_border==STOCK_FALLBACK) {
            memcpy(expected[3], (uint8_t[]){0,0,128,128}, 4);
            memset(expected[7], 0, 4);
        } else {
            memcpy(expected[3], (uint8_t[]){48,16,207,191}, 4);
            memcpy(expected[7], (uint8_t[]){96,32,159,127}, 4);
        }
    }
    for (int gpu=0;gpu<4;gpu++) for (int pass = 0; pass < 8; ++pass) {
        if (custom_border && pass!=3 && pass!=7) continue;
        list_t list = {0}; uint8_t pixels[64] = {0};
        struct { uint32_t index; float coordinate; uint32_t pad[2]; } parameters = {pass == 2 ? index+24 : index, pass == 3 || pass == 7 ? 1.5f : 0.5f, {0}};
        if (custom_border && pass==3) parameters.coordinate=1.0f;
        for (int i = 0; i < 3; ++i) {
            p_clear_tex clear = {.tex = &textures[i], .r = {.width = 1, .height = 1, .depth = 1}, .mask = 15};
            clear.c.f[i == 0 ? 1 : i == 1 ? 2 : 0] = 1; clear.c.f[3] = 1;
            put(&list, OP_CLEAR_TEX, &clear, sizeof clear);
        }
        RECORD(OP_SET_TEX_POOL, p_set_pool, .pool = pass == 6 ? &alternate_texture_pool : &texture_pool);
        RECORD(OP_SET_SAMP_POOL, p_set_pool, .pool = pass == 7 ? &alternate_sampler_pool : &sampler_pool);
        struct { uint64_t address, size, value; } update = {(uintptr_t)&handles[44 + index], 8, pass >= 6 ? 5u : pass >= 4 ? 7u : pass ? 6u : 5u};
        if (table) { // Multiple logical indices can name the same physical descriptor.
            handles[44+5]=5; samplers[16+5]=1u<<20;
            handles[44+27]=5; samplers[16+27]=1u<<20;
        }
        if (table == 2) {
            uint64_t fixed = (pass == 3 ? 258ull : pass == 7 ? 513ull : 257ull) << 20;
            RECORD(OP_BIND_SAMP,p_bind_handles,.stage=1,.first=1,.n=1,.h={fixed});
        }
        if (!gpu) put(&list, OP_UPDATE_UB, &update, sizeof update);
        samplers[16 + index] = (uint64_t)(pass == 3 ? 2 : 1) << 20;
        if (table == 2) samplers[16+index] = 3ull << 20; // deliberately wrong in-table sampler
        if(gpu) {
            uint32_t values[4]={(uint32_t)update.value,0,0,0};
            handles[44+index]=0;
            push_constants(&list,1,240,values,sizeof values);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=update.address,.size=8);
            RECORD(OP_DISPATCH,p_dispatch,.x=2,.y=1,.z=1);
            values[0]=(uint32_t)samplers[16+index];samplers[16+index]=0;
            push_constants(&list,1,240,values,sizeof values);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)&samplers[16+index],.size=8);
            RECORD(OP_DISPATCH,p_dispatch,.x=2,.y=1,.z=1);
            if(gpu==2) { put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);mtl_sub_commit();list.length=0; }
            if(gpu==3) {
                // Two later known ranges cover the earlier GPU-written handle;
                // the sampler is a known fill. Neither needs a submission cut.
                struct { uint64_t address,size;uint32_t value,pad; } part={update.address,4,(uint32_t)update.value,0};
                put(&list,OP_UPDATE_UB,&part,sizeof part);
                part.address+=4;part.value=0;put(&list,OP_UPDATE_UB,&part,sizeof part);
                RECORD(OP_CLEAR_BUF,p_clear_buf,.addr=(uintptr_t)&samplers[16+index],.size=8,.value=values[0]);
            }
        }
        push_constants(&list, 0, 240, white, sizeof white);
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = table ? &table_program[table-1] : &bindless_program, .stages = 3);
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
        uint64_t before=mtl_last_fence();
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        if(!gpu || gpu==3) assert(mtl_last_fence()==before+1);
        for (size_t i = 0; i < sizeof pixels; ++i) {
            int half_unorm = custom_border==STOCK_FALLBACK && pass==3 && i%4>=2;
            if (pixels[i]==expected[pass][i%4] || (half_unorm && pixels[i]==127)) continue;
            fprintf(stderr,"uniform texture array GPU%d pass %d pixel %zu channel %zu: %u expected %u\n",
                    gpu,pass,i/4,i%4,pixels[i],expected[pass][i%4]);
            abort();
        }
    }
    mtl_pool_del(handles); mtl_pool_del(samplers);
    mtl_reg_tex(4101, NULL, NULL);
    for (int i = 0; i < 3; ++i) { mtl_reg_tex(2053 + i, NULL, NULL); mtl_tex_release(&textures[i]); }
    mtl_tex_release(&output);
    if (table) puts("Sparse NVN uniform handle tables passed");
    else puts("Uniform texture arrays, GPU-produced handles, known overwrites without waits, pool bases and independent samplers passed");
}

// A single guest sampler is used with three different numerical types in one dispatch.
static void typed_sampler_border(int native, int separate) {
    NVNformat formats[] = {NVN_FORMAT_RGBA32I,NVN_FORMAT_RGBA32UI,NVN_FORMAT_RGBA32F};
    tex_t textures[3];
    for (int i=0; i<3; ++i) {
        textures[i] = (tex_t){.serial=0xe530+i,.d={.width=1,.height=1,.depth=1,.levels=1,.samples=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=formats[i]}};
        nvn_tex_layout(&textures[i].d,&textures[i].L);
        mtl_reg_tex(1410+i,&textures[i],NULL);
    }
    samp_par sampler = {.ws=NVN_WRAP_MODE_CLAMP_TO_BORDER,.border={-7.5f,2,129,1}};
    mtl_reg_samp(1417,&sampler);
    struct { int32_t s[4]; uint32_t u[4]; float f[4]; } result;
    memset(&result,0xcc,sizeof result); mtl_pool_add(&result,sizeof result);
    list_t list={0};
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&typed_border_program[separate],.stages=0x20);
    for (uint32_t i=0; i<3; ++i)
        RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=i,.h=(1417ull<<20)|(1410+i));
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)&result,.size=sizeof result);
    RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
    put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int i=0; i<4; ++i) {
        float f=native ? sampler.border[i] : (float)(i==3);
        int32_t si=(int32_t)f; uint32_t ui=f>0 ? (uint32_t)f : 0;
        if (result.s[i]!=si || result.u[i]!=ui || result.f[i]!=f) {
            fprintf(stderr,"sampler border channel %d got %d %u %g expected %d %u %g\n", i,result.s[i],result.u[i],result.f[i],si,ui,f);
            abort();
        }
    }
    for (int i=0; i<3; ++i) { mtl_reg_tex(1410+i,NULL,NULL); mtl_tex_release(&textures[i]); }
    mtl_pool_del(&result);
}

static void custom_borders(void) {
    int native;
    replay_features(&native,NULL);
    typed_sampler_border(native,0); typed_sampler_border(native,1);
    for (int table=0; table<3; ++table) uniform_texture_array(table,native ? CUSTOM_BORDER : STOCK_FALLBACK);
    puts(native ? "Custom sampler borders: float/sint/uint, linear edge and full border through fixed and indexed bindings passed"
                : "Sampler fallback: typed stock colors through fixed and indexed bindings passed");
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
    for (test_nvn2 = 0; test_nvn2 < 2; test_nvn2++) for (int mode = 0; mode < 5; ++mode) {
        uint32_t output[20] = {0}, uniform[16] = {0}, counts[] = {mode >= 2 ? 0u : 2u, mode == 2 ? 0u : 3u, mode == 2 ? 0u : 4u};
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
        RECORD(OP_DISPATCH, p_dispatch, .x = mode ? 9 : 2, .y = mode ? 8 : 3, .z = mode ? 7 : 4, .ind = mode ? (uintptr_t)counts : 0);
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        uint32_t expected[] = {2,3,4,0, 2,3,4,0,
            (uint32_t)(uintptr_t)uniform, (uint32_t)((uint64_t)(uintptr_t)uniform >> 32), sizeof uniform, 0,
            (uint32_t)(uintptr_t)output, (uint32_t)((uint64_t)(uintptr_t)output >> 32), sizeof output, 0,
            test_nvn2 ? 2u : 0u, test_nvn2 ? 3u : 0u, test_nvn2 ? 4u : 0u, 0};
        for (int i = 0; i < 20; ++i) if (output[i] != expected[i]) {
            fprintf(stderr, "compute driver NVN2%d mode %d word %d: %#x expected %#x\n", test_nvn2, mode, i, output[i], expected[i]); abort();
        }
    }
    test_nvn2 = 0;
    puts("Compute driver dimensions and buffer tables match direct, CPU and GPU indirect dispatches on both guest ABIs");
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
// Neither row padding nor the gap after a slice belongs to the transfer.
// Protect the gaps and change one source word on the GPU before uploading it.
static void guarded_texture_transfers(void) {
    size_t page = sysconf(_SC_PAGESIZE), row = 2*page, image = 4*page-4;
    uint8_t *mapping = mmap(NULL,8*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mapping!=MAP_FAILED);
    for (size_t i=1; i<8; i+=2) assert(mprotect(mapping+i*page,page,PROT_NONE)==0);
    uint8_t *src = mapping+page-12;
    for (int volume=0; volume<=1; ++volume) {
        tex_t t={.serial=0x8fba00+volume,.d={.width=8,.height=8,.depth=8,.levels=2,
                 .target=volume?NVN_TEXTURE_TARGET_3D:NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_R32UI}};
        nvn_tex_layout(&t.d,&t.L);
        uint32_t initial[4][4][4]={0}, expected[4][4][4]={0}, actual[4][4][4]={0};
        for (unsigned z=0; z<2; ++z) for (unsigned y=0; y<2; ++y) for (unsigned x=0; x<3; ++x) {
            uint32_t value = 1+100*z+10*y+x;
            memcpy(src+z*image+y*row+4*x,&value,4);
            expected[z+2][y+1][x+1] = value;
        }
        uint32_t gpu[4] = {0x87654321,0,0,0};
        expected[3][1][2] = gpu[0];
        view_t mip={.set_levels=1,.base_level=1,.nlevels=1}, upload=mip;
        if (!volume) { upload.set_layers=1; upload.min_layer=1; upload.nlayers=3; }
        list_t list={0};
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&t,.view=mip,.has_view=1,.src=(uintptr_t)initial,.r={.width=4,.height=4,.depth=4});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
        push_constants(&list,1,240,gpu,sizeof gpu);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)(src+image+4),.size=4);
        RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        RECORD(OP_COPY_STRIDE,p_copy_stride,.stride=row);
        RECORD(OP_COPY_STRIDE,p_copy_stride,.image=1,.stride=image);
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&t,.view=upload,.has_view=1,.src=(uintptr_t)src,
               .r={.xoffset=1,.yoffset=1,.zoffset=volume?2:1,.width=3,.height=2,.depth=2});
        RECORD(OP_COPY_STRIDE,p_copy_stride,.stride=0);
        RECORD(OP_COPY_STRIDE,p_copy_stride,.image=1,.stride=0);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&t,.view=mip,.has_view=1,.src=(uintptr_t)actual,.r={.width=4,.height=4,.depth=4});
        put(&list,OP_END,NULL,0);
        mtl_queue_activate(0x8fba00+volume);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        assert(!memcmp(expected,actual,sizeof expected));
        mtl_tex_release(&t); mtl_queue_forget(0x8fba00+volume);
    }
    assert(munmap(mapping,8*page)==0);
    puts("Vulkan guarded pitched array and volume transfers preserved GPU and CPU source rows");
}

// BC endpoints that are not selected still belong to the guest bytes. A
// round-trip through decoded pixels cannot reconstruct them.
static void compressed_transfers(void) {
    tex_t blocks={.serial=0x8fbd00,.has_layout=1,.d={.width=16,.height=8,.depth=3,.levels=2,
        .target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_RGBA_DXT1}};
    tex_t color={.serial=0x8fbd01,.d={.width=8,.height=4,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&blocks.d,&blocks.L); nvn_tex_layout(&color.d,&color.L);
    blocks.mem=calloc(1,blocks.L.size); assert(blocks.mem);
    uint32_t source[12]={0}, expected[3][4]={{0x001ff800,0,0x123407e0,0},{0x0077f800,0,0x345607e0,0},{0x993307e0,0,0x1122f800,0}};
    assert(!nvn_swizzle(&blocks.d,1,0,expected[0],16,blocks.mem));
    memcpy(source,expected[1],16); memcpy(source+7,expected[2],16); source[7]=0;
    view_t slice={.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=1,.nlayers=2};
    for (int i=0; i<2; ++i) {
        view_t sample=slice; sample.set_target=1; sample.target=NVN_TEXTURE_TARGET_2D;
        sample.min_layer=1+i; sample.nlayers=1; mtl_reg_tex(990+i,&blocks,&sample);
    }
    uint8_t rgba[2][32][4]={0};
    uint32_t returned[3][4]={{0}}, forwarded[3][4]={{0}};
    list_t list={0};
    // An earlier fill to the destination's physical block must not publish
    // over the later texture copy when this submission completes.
    uint8_t *alias=blocks.mem + blocks.L.layer_size + nvn_tex_level_offset(&blocks.d,1);
    RECORD(OP_CLEAR_BUF,p_clear_buf,.addr=(uintptr_t)alias,.size=4,.value=0xcafebabe);
    RECORD(OP_CLEAR_BUF,p_clear_buf,.addr=(uintptr_t)(source+7),.size=4,.value=expected[2][0]);
    RECORD(OP_COPY_STRIDE,p_copy_stride,.stride=24);
    RECORD(OP_COPY_STRIDE,p_copy_stride,.image=1,.stride=28);
    RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&blocks,.has_view=1,.view=slice,.src=(uintptr_t)source,.r={.width=8,.height=4,.depth=2});
    RECORD(OP_COPY_STRIDE,p_copy_stride,.stride=0);
    RECORD(OP_COPY_STRIDE,p_copy_stride,.image=1,.stride=0);
    view_t mip={.set_levels=1,.base_level=1,.nlevels=1};
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&blocks,.has_view=1,.view=mip,.src=(uintptr_t)returned,.r={.width=8,.height=4,.depth=3});
    uint64_t copy[]={(uintptr_t)returned,(uintptr_t)forwarded,sizeof returned}; put(&list,OP_COPY_B2B,copy,sizeof copy);
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color});
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&feedback_program,.stages=3);
    for (int i=0; i<2; ++i) {
        RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1,.slot=0,.h=990+i);
        RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1,.slot=1,.h=990+i);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)rgba[i],.r={.width=8,.height=4,.depth=1});
    }
    put(&list,OP_END,NULL,0);
    mtl_queue_activate(0x8fbd00); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    assert(!memcmp(expected,returned,sizeof expected) && !memcmp(expected,forwarded,sizeof expected));
    for (int z=0; z<3; ++z) {
        uint32_t pool[4]; assert(!nvn_deswizzle(&blocks.d,1,z,blocks.mem,pool,16));
        assert(!memcmp(expected[z],pool,sizeof pool));
    }
    for (unsigned z=0; z<2; ++z) for (int i=0; i<32; ++i) {
        int red = (i%8<4) != z;
        uint8_t want[]={red?255:0,red?255:0,red?0:255,255};
        assert(!memcmp(rgba[z][i],want,4));
    }
    // A texture copy must preserve the encoded bytes too. Replace only
    // the second block of the middle layer. Its first block stays green.
    tex_t dest=blocks; dest.serial+=2; dest.mem=calloc(1,dest.L.size); assert(dest.mem);
    uint32_t after[3][4], copied_blocks[3][4]={{0}};
    for (unsigned z=0; z<3; ++z) {
        memcpy(after[z],expected[2],8); memcpy(after[z]+2,expected[0]+2,8);
        assert(!nvn_swizzle(&dest.d,1,z,after[z],16,dest.mem));
    }
    memcpy(after[1]+2,expected[1],8);
    view_t selected=slice; selected.target=NVN_TEXTURE_TARGET_2D; selected.set_target=1; selected.nlayers=1;
    mtl_reg_tex(992,&dest,&selected);
    list.length=0;
    RECORD(OP_COPY_T2T,p_copy_t2t,.src=&blocks,.dst=&dest,.has_sv=1,.sv=slice,.has_dv=1,.dv=slice,
           .sr={.width=4,.height=4,.depth=1},.dr={.xoffset=4,.width=4,.height=4,.depth=1});
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&dest,.view=mip,.has_view=1,.src=(uintptr_t)copied_blocks,.r={.width=8,.height=4,.depth=3});
    RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1,.slot=0,.h=992);
    RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1,.slot=1,.h=992);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)rgba[0],.r={.width=8,.height=4,.depth=1});
    put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    assert(!memcmp(copied_blocks,after,sizeof after));
    for (unsigned i=0; i<32; ++i) {
        uint8_t want[]={i%8<4?0:255,i%8<4?0:255,i%8<4?255:0,255};
        assert(!memcmp(want,rgba[0][i],4));
    }
    mtl_reg_tex(992,NULL,NULL); mtl_tex_release(&dest); free(dest.mem);
    mtl_reg_tex(990,NULL,NULL); mtl_reg_tex(991,NULL,NULL);
    mtl_tex_release(&color); mtl_tex_release(&blocks); free(blocks.mem); mtl_queue_forget(0x8fbd00);
    puts("Vulkan compressed block transfers preserved guest bytes, decoded pixels and same-list consumers");
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

// A nested program binding must reach a draw recorded by the caller. Reading
// fragment depth into R32F also distinguishes explicit zero from default far.
static void nested_depth_ranges(void) {
    tex_t target = {.magic = MAGIC('T','E','X',0), .serial = 0x8102, .d = {.width = 8, .height = 2, .depth = 1,
        .levels = 1, .samples = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F}};
    nvn_tex_layout(&target.d, &target.L);
    list_t inner = {0}, middle = {0}, list = {0};
    p_bind_program program_bind = {.prog = &depth_range_program, .stages = 3};
    put(&inner, OP_BIND_PROGRAM, &program_bind, sizeof program_bind);
    volatile uint32_t event = 0;
    struct { volatile uint32_t *dst; uint32_t mode, value; } signal = {&event, NVN_EVENT_SIGNAL_MODE_WRITE, 7};
    put(&inner, OP_SIGNAL_EVENT, &signal, sizeof signal);
    put(&inner, OP_END, NULL, 0);
    const uint8_t *child = inner.bytes;
    put(&middle, OP_CALL, &child, sizeof child);
    signal.mode = NVN_EVENT_SIGNAL_MODE_ADD; signal.value = 4;
    put(&middle, OP_SIGNAL_EVENT, &signal, sizeof signal);
    put(&middle, OP_END, NULL, 0);
    float pixels[16] = {0};
    RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&target});
    RECORD(OP_CLEAR_COLOR, p_clear_color, .mask = 15, .rgba = {0.5f, 0, 0, 1});
    child = middle.bytes;
    put(&list, OP_CALL, &child, sizeof child);
    const float ranges[][2] = {{0,1}, {1,0}, {0,0}};
    for (int i = 0; i < 3; ++i) {
        RECORD(OP_VIEWPORTS, p_viewports, .n = 1, .v = {{2*i, 0, 2, 2}});
        RECORD(OP_DEPTH_RANGE, p_depth_range, .n = ranges[i][0], .f = ranges[i][1]);
        RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
    }
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &target, .src = (uintptr_t)pixels,
        .r = {.width = 8, .height = 2, .depth = 1});
    put(&list, OP_END, NULL, 0);
    mtl_queue_activate(0x8102);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    assert(event == 11);
    const float expected[] = {0.25f, 0.75f, 0, 0.5f};
    for (int i = 0; i < 16; ++i) {
        float want = expected[(i % 8) / 2];
        if (pixels[i] != want) die("viewport depth at pixel %d: got %g, expected %g", i, pixels[i], want);
    }
    mtl_tex_release(&target);
    mtl_queue_forget(0x8102);
    puts("Vulkan nested program binding, reversed depth and zero far depth passed");
}

// The triangle crosses both depth planes. At the four sample centres its
// interpolated depths are -0.25, 0.25, 0.75 and 1.25. Clamping vertices
// before interpolation would also change the two pixels inside the volume.
static void depth_clamp(void) {
    tex_t depth = {.serial=0x8103, .d={.width=4,.height=4,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_DEPTH32F}};
    nvn_tex_layout(&depth.d, &depth.L);
    const float vertices[][3] = {{-1,-1,-0.5f},{3,-1,3.5f},{-1,3,-0.5f}};
    float pixels[16] = {0};
    list_t list = {0};
    RECORD(OP_SET_RT,p_set_rt,.depth=&depth);
    RECORD(OP_CLEAR_DS,p_clear_ds,.depth=0.5f,.depth_mask=1);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_clamp_program,.stages=3);
    va_st va = {.fmt=NVN_FORMAT_RGB32F};
    p_bind_vattr attr = {.n=1};
    memcpy(attr.st, &va, sizeof va);
    put(&list, OP_BIND_VATTR, &attr, sizeof attr);
    RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=1,.st={{.stride=12}});
    RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=sizeof vertices,.addr=(uintptr_t)vertices);
    for (int y=0; y<4; ++y) {
        RECORD(OP_SCISSOR,p_scissor,.y=y,.w=4,.h=1);
        RECORD(OP_DEPTH_CLAMP,p_depth_clamp,.on=y==1 || y==2);
        ds_st ds = {.dt=1, .dw=1, .df=y<2 ? NVN_COMPARE_FUNC_ALWAYS : NVN_COMPARE_FUNC_LESS};
        p_bind_state state = {.kind=5};
        memcpy(state.bytes, &ds, sizeof ds);
        put(&list, OP_BIND_STATE, &state, sizeof state);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    }
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&depth,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1});
    put(&list, OP_END, NULL, 0);
    mtl_queue_activate(0x8103);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    const float expected[][4] = {{.5f,.25f,.75f,.5f},{0,.25f,.75f,1},{0,.25f,.5f,.5f},{.5f,.25f,.5f,.5f}};
    for (int y=0; y<4; ++y) for (int x=0; x<4; ++x) {
        float got = pixels[4*y+x];
        int row = getenv("DM_VK_NO_DEPTH_CLAMP") ? (y < 2 ? 0 : 3) : y;
        if (got != expected[row][x]) die("depth clamp at (%d,%d): %g expected %g", x,y,got,expected[row][x]);
    }
    mtl_tex_release(&depth);
    mtl_queue_forget(0x8103);
    puts(getenv("DM_VK_NO_DEPTH_CLAMP") ? "Vulkan unavailable depth clamp uses the reported clip path" :
         "Vulkan depth clamp preserves interpolated depth, near/far coverage and depth-test ordering");
}

static void conditional_rendering(void) {
    if (!DM_GAME_RENDER_COND) return;  // The recorder emits these commands for opt-in profiles.
    static uint64_t same[4] = {7,0,7,0}, different[4] = {9,0,2,0}, timestamp[4] = {1}, query[4];
    tex_t color = {.serial = 0x8115, .d = {.width = 8, .height = 2, .depth = 1, .levels = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F}};
    nvn_tex_layout(&color.d, &color.L);
    tex_t depth = color, sampled = color;
    depth.serial++; sampled.serial += 2; depth.d.height = 8; depth.d.format = NVN_FORMAT_DEPTH32F;
    nvn_tex_layout(&depth.d, &depth.L);
    float pixels[2][8] = {{0}};
    mtl_queue_activate(0x8115);
    list_t list = {0};
    RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&color}, .depth = &depth);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .mask = 15, .rgba = {1});
    RECORD(OP_CLEAR_DS, p_clear_ds, .depth = 1, .depth_mask = 1);
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &depth_range_program, .stages = 3);
    uint64_t report[][2] = {{NVN_COUNTER_TYPE_TIMESTAMP_TOP, (uintptr_t)timestamp},
                            {NVN_COUNTER_TYPE_SAMPLES_PASSED, (uintptr_t)query}};
    list_t seed = {0};
    for (unsigned i = 0; i < 2; i++) put(&seed, OP_REPORT_COUNTER, report[i], sizeof report[i]);
    put(&seed, OP_END, NULL, 0);
    const p_render_cond conditions[] = {{3}, {2}, {.mode=0,.addr=(uintptr_t)same},
        {.mode=0,.addr=(uintptr_t)different}, {.mode=1,.addr=(uintptr_t)different}, {.mode=1,.addr=(uintptr_t)same},
        {.mode=1,.addr=(uintptr_t)timestamp}, {.mode=1,.addr=(uintptr_t)query}};
    const int enabled[] = {0,1,1,0,1,0,0,1};
    uint16_t indices[] = {0,1,2};
    for (int x = 0; x < 8; x++) {
        put(&list, OP_RENDER_COND, &conditions[x], sizeof conditions[x]);
        RECORD(OP_SCISSOR, p_scissor, .x=x, .y=0, .w=1, .h=1);
        RECORD(OP_CLEAR_COLOR, p_clear_color, .mask=1, .rgba={.5f});
        RECORD(OP_CLEAR_DS, p_clear_ds, .depth=.25f, .depth_mask=1);
        RECORD(OP_SCISSOR, p_scissor, .x=x, .y=1, .w=1, .h=1);
        if (x & 1) {
            RECORD(OP_DRAW_IDX, p_draw_idx, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .itype=1, .count=3,
                   .instances=1, .ibuf=(uintptr_t)indices);
        } else {
            RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .count=3, .instances=1);
        }
    }
    RECORD(OP_RENDER_COND, p_render_cond, .mode=3);
    // A deferred handle read of freshly copied GPU bytes cuts the native
    // command buffer. NVN still sees one submission with rendering disabled.
    uint64_t handle = 0;
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&color, .src=(uintptr_t)&handle, .r={.width=2,.height=1,.depth=1});
    RECORD(OP_BIND_TEX, p_bind_handles, .stage=1,.first=5,.n=1,.defer=1,.src=(uintptr_t)&handle);
    RECORD(OP_SCISSOR, p_scissor, .w=8,.h=2);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .mask=15,.rgba={.125f});
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&color, .src=(uintptr_t)pixels, .r={.width=8,.height=2,.depth=1});
    put(&list, OP_END, NULL, 0);
    mtl_sub_begin();
    mtl_exec(seed.bytes);
    uint64_t additional[600][2];
    for (unsigned i = 0; i < 600; i++) {
        list_t another = {0};
        uint64_t entry[] = {report[i % 2][0], (uintptr_t)additional[i]};
        put(&another, OP_REPORT_COUNTER, entry, sizeof entry); put(&another, OP_END, NULL, 0);
        mtl_exec(another.bytes);
    }
    mtl_exec(list.bytes);
    assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    for (int x=0; x<8; x++) {
        if (pixels[0][x] != (enabled[x] ? .5f : 1) || pixels[1][x] != (enabled[x] ? .25f : 1))
            die("conditional clear/draw column %d: %g / %g", x, pixels[0][x], pixels[1][x]);
    }
    // Enable is reset per submission. Timestamp comparisons return to guest
    // memory, and a samples-passed address remains unknown during replay.
    timestamp[0] = 4;
    list = (list_t){0};
    RECORD(OP_SCISSOR, p_scissor, .w=8, .h=2);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .mask=15, .rgba={0});
    RECORD(OP_RENDER_COND, p_render_cond, .mode=1, .addr=(uintptr_t)timestamp);
    RECORD(OP_SCISSOR, p_scissor, .w=1, .h=2);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .mask=1, .rgba={.75f});
    RECORD(OP_RENDER_COND, p_render_cond, .mode=1, .addr=(uintptr_t)query);
    RECORD(OP_SCISSOR, p_scissor, .x=1, .w=1, .h=2);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .mask=1, .rgba={.5f});
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&color, .src=(uintptr_t)pixels, .r={.width=8,.height=2,.depth=1});
    put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    for (int y=0; y<2; y++) for (int x=0; x<8; x++) assert(pixels[y][x] == (x==0 ? .75f : x==1 ? .5f : 0));
    list = (list_t){0};
    samp_par nearest = {0};
    mtl_reg_tex(908, &depth, NULL); mtl_reg_samp(908, &nearest);
    RECORD(OP_SET_RT, p_set_rt, .n=1, .color={&sampled});
    RECORD(OP_SCISSOR, p_scissor, .w=8, .h=2);
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&depth_copy_program, .stages=3);
    RECORD(OP_BIND_TEX1, p_bind_tex1, .stage=1|BIND_TEX1_NOBASE, .slot=0, .h=908|(908ull<<20));
    RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .count=3, .instances=1);
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&sampled, .src=(uintptr_t)pixels, .r={.width=8,.height=2,.depth=1});
    put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    for (int x=0; x<8; x++) assert(pixels[0][x] == (enabled[x] ? .25f : 1) && pixels[1][x] == 1);
    mtl_reg_tex(908, NULL, NULL);
    mtl_tex_release(&sampled); mtl_tex_release(&depth); mtl_tex_release(&color); mtl_queue_forget(0x8115);
    puts("Vulkan conditional clears and draws matched all four modes, pending timestamps and unknown reports");
}

static void multisample_colors(void) {
    tex_t source = {.serial=0x8120, .d={.width=4,.height=4,.depth=2,.levels=1,.samples=4,
        .target=NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY,.format=NVN_FORMAT_RGBA8}};
    tex_t result = source;
    result.serial++; result.d.target=NVN_TEXTURE_TARGET_2D_ARRAY; result.d.samples=1;
    nvn_tex_layout(&source.d, &source.L); nvn_tex_layout(&result.d, &result.L);
    view_t layer = {.set_layers=1,.min_layer=1,.nlayers=1};
    list_t list = {0};
    uint8_t pixels[2][4][4][4];
    RECORD(OP_CLEAR_TEX, p_clear_tex, .tex=&result,.r={.width=4,.height=4,.depth=2},.mask=15,.c.f={1,0,1,1});
    RECORD(OP_SET_RT, p_set_rt, .n=1,.color={&source},.has_cview={1},.cview={layer});
    RECORD(OP_CLEAR_COLOR, p_clear_color, .mask=15,.rgba={0,0,1,1});
    RECORD(OP_SCISSOR, p_scissor, .w=4,.h=2);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .mask=2,.rgba={1,1,0,0});
    RECORD(OP_SCISSOR, p_scissor, .x=1,.w=2,.h=4);
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&one_sample_program,.stages=3);
    RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    RECORD(OP_COPY_T2T, p_copy_t2t, .src=&source,.dst=&result,.flags=DM_COPY_RESOLVE,
           .has_sv=1,.sv=layer,.has_dv=1,.dv=layer,.sr={.width=4,.height=4,.depth=1},.dr={.width=4,.height=4,.depth=1});
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&result,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=2});
    put(&list, OP_END, NULL, 0); mtl_queue_activate(0x8120);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    for (int z=0; z<2; z++) for (int y=0; y<4; y++) for (int x=0; x<4; x++) {
        int drawn = z && x >= 1 && x < 3;
        uint8_t expected[] = {z ? (drawn ? 64 : 0) : 255, z && y < 2 ? (drawn ? 191 : 255) : 0, drawn ? 191 : 255, 255};
        if (memcmp(expected, pixels[z][y][x], 4)) die("4x resolve layer %d at %d,%d: %u,%u,%u,%u",
            z,x,y,pixels[z][y][x][0],pixels[z][y][x][1],pixels[z][y][x][2],pixels[z][y][x][3]);
    }
    mtl_tex_release(&source); mtl_tex_release(&result); mtl_queue_forget(0x8120);
    puts("Vulkan 4x resolve averaged one red sample and preserved the adjacent array layer");
}

static void multisample_depth(void) {
    tex_t source = {.serial=0x8122,.d={.width=8,.height=8,.depth=1,.levels=1,.samples=4,
        .target=NVN_TEXTURE_TARGET_2D_MULTISAMPLE,.format=NVN_FORMAT_DEPTH32F}};
    tex_t resolved = source, output = source;
    resolved.serial++; resolved.d.samples=1; resolved.d.target=NVN_TEXTURE_TARGET_2D;
    output.serial+=2; output.d.samples=1; output.d.target=NVN_TEXTURE_TARGET_2D; output.d.format=NVN_FORMAT_RGBA8;
    nvn_tex_layout(&source.d,&source.L); nvn_tex_layout(&resolved.d,&resolved.L); nvn_tex_layout(&output.d,&output.L);
    samp_par nearest = {0};
    mtl_reg_tex(909,&resolved,NULL); mtl_reg_samp(909,&nearest);
    list_t list = {0};
    uint8_t pixels[8][8][4];
    RECORD(OP_CLEAR_TEX, p_clear_tex, .tex=&resolved,.r={.width=8,.height=8,.depth=1},.mask=15,.c.f={0});
    RECORD(OP_SET_RT, p_set_rt, .depth=&source);
    RECORD(OP_CLEAR_DS, p_clear_ds, .depth=.75f,.depth_mask=1);
    RECORD(OP_SCISSOR, p_scissor, .x=2,.y=1,.w=3,.h=4);
    RECORD(OP_CLEAR_DS, p_clear_ds, .depth=.5f,.depth_mask=1);
    // Change sample 0 in even columns and sample 1 in odd columns. Neither
    // minimum, maximum nor average can reproduce sample zero throughout.
    RECORD(OP_SCISSOR, p_scissor, .w=8,.h=8);
    p_bind_state state={.kind=5};
    ds_st ds={.dt=1,.dw=1,.df=NVN_COMPARE_FUNC_ALWAYS};
    memcpy(state.bytes,&ds,sizeof ds); put(&list,OP_BIND_STATE,&state,sizeof state);
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&one_sample_program,.stages=3);
    RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    RECORD(OP_COPY_T2T, p_copy_t2t, .src=&source,.dst=&resolved,.flags=DM_COPY_RESOLVE,
           .sr={.width=8,.height=8,.depth=1},.dr={.width=8,.height=8,.depth=1});
    RECORD(OP_SET_RT, p_set_rt, .n=1,.color={&output});
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&depth_copy_program,.stages=3);
    RECORD(OP_BIND_TEX1, p_bind_tex1, .stage=1|BIND_TEX1_NOBASE,.slot=0,.h=909|(909ull<<20));
    RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
    put(&list,OP_END,NULL,0); mtl_queue_activate(0x8122);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int y=0;y<8;y++) for (int x=0;x<8;x++) {
        int expected = x % 2 == 0 ? 64 : x>=2 && x<5 && y>=1 && y<5 ? 128 : 191;
        if (abs((int)pixels[y][x][0]-expected)>1) die("sample-zero depth resolve at %d,%d: %u",x,y,pixels[y][x][0]);
    }
    mtl_reg_tex(909,NULL,NULL); mtl_tex_release(&source); mtl_tex_release(&resolved); mtl_tex_release(&output);
    mtl_queue_forget(0x8122);
    puts("Vulkan depth resolve selects sample zero, including a scissored depth clear");
}

static void visibility_reports(void) {
    tex_t color={.serial=0x8125,.d={.width=8,.height=8,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&color.d,&color.L);
    tex_t depth=color; depth.serial=0x8127; depth.d.format=NVN_FORMAT_DEPTH32F;
    nvn_tex_layout(&depth.d,&depth.L);
    uint64_t reports[7][2] = {{0}}, type=NVN_COUNTER_TYPE_SAMPLES_PASSED;
    mtl_queue_activate(0x8125);
    list_t list = {0};
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color},.depth=&depth);
    RECORD(OP_CLEAR_DS,p_clear_ds,.depth=0,.depth_mask=1);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_range_program,.stages=3);
    for (int i=0;i<7;i++) {
        if (i==0 || i==3) put(&list,OP_RESET_COUNTER,&type,sizeof type);
        // One visible 4x4 rectangle, then no fragments, then another 4x4.
        // A masked clear is rasterized internally but adds no guest samples.
        ds_st ds={.dt=1,.df=i==1 || i==3 ? NVN_COMPARE_FUNC_LESS : NVN_COMPARE_FUNC_ALWAYS};
        p_bind_state state={.kind=5}; memcpy(state.bytes,&ds,sizeof ds);
        put(&list, OP_BIND_STATE, &state, sizeof state);
        RECORD(OP_SCISSOR,p_scissor,.x=2,.y=1,.w=4,.h=4);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        RECORD(OP_SCISSOR,p_scissor,.w=8,.h=8);
        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=1,.rgba={1});
        uint64_t report[] = {type,(uintptr_t)reports[i]};
        put(&list,OP_REPORT_COUNTER,report,sizeof report);
        if (i==1 || i==4 || i==5 || i==6) {
            put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes);
            uint64_t fence=mtl_sub_commit();
            if (i==6) { mtl_queue_forget(0x8125); assert(mtl_wait(fence,UINT64_MAX)); }
            list=(list_t){0};
        }
        if (i==4) mtl_queue_activate(0x8126);
        if (i==5) mtl_queue_activate(0x8125);
    }
    const unsigned expected[] = {16,16,32,0,16,0,32};
    int precise = replay_features(NULL,NULL).occlusionQueryPrecise;
    for (unsigned i=0;i<7;i++)
        if ((precise ? reports[i][0]!=expected[i] : !!reports[i][0]!=!!expected[i]) || !reports[i][1]) die("visibility report %u: got %llu, expected %u",i,
            (unsigned long long)reports[i][0],expected[i]);
    mtl_queue_forget(0x8126);
    // A busy pass uses a second query page; a recycled submission context
    // must reset its old slots and read back only the pages used this time.
    uint64_t many[257][2];
    mtl_queue_activate(0x8128);
    for (int round=0;round<4;round++) {
        unsigned count = round ? 3 : 257;
        list=(list_t){0};
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_range_program,.stages=3);
        RECORD(OP_SCISSOR,p_scissor,.w=1,.h=1);
        put(&list,OP_RESET_COUNTER,&type,sizeof type);
        put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes);
        for (unsigned i=0;i<count;i++) {
            list=(list_t){0};
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            uint64_t report[]={type,(uintptr_t)many[i]};
            put(&list,OP_REPORT_COUNTER,report,sizeof report); put(&list,OP_END,NULL,0); mtl_exec(list.bytes);
        }
        assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (unsigned i=0;i<count;i++) assert(precise ? many[i][0] == i+1 : many[i][0] > 0);
    }
    mtl_queue_forget(0x8128); mtl_tex_release(&color); mtl_tex_release(&depth);
    puts("Vulkan visibility counters accumulate only guest samples and survive pass, submission and queue changes");
}

static void segment_timestamps(void) {
    const char *mode=getenv("DM_NVN_GPU_TS");
    int segment = mode && *mode ? !strcmp(mode,"segment") : DM_GAME_GPU_TS_SEGMENT;
    tex_t color={.serial=0x8129,.d={.width=128,.height=128,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&color.d,&color.L);
    NVNcounterData reports[4] = {0};
    list_t list={0};
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color});
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_range_program,.stages=3);
    for (int i=0;i<4;i++) {
        uint64_t report[]={i % 2 ? NVN_COUNTER_TYPE_TIMESTAMP : NVN_COUNTER_TYPE_TIMESTAMP_TOP,(uintptr_t)&reports[i]};
        put(&list,OP_REPORT_COUNTER,report,sizeof report);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    }
    put(&list,OP_END,NULL,0); mtl_queue_activate(0x8129);
    uint64_t lo=nvn_timestamp_from_ns(guest_ns());
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    uint64_t hi=nvn_timestamp_from_ns(guest_ns());
    if (reports[0].timestamp!=reports[2].timestamp || reports[1].timestamp!=reports[3].timestamp ||
        (segment ? reports[0].timestamp>=reports[1].timestamp : reports[0].timestamp!=reports[1].timestamp) ||
        reports[0].timestamp<lo || reports[1].timestamp>hi)
        die("GPU %s timestamp boundaries did not match their guest clock interval", segment ? "segment" : "submission");
    for (int i=0;i<4;i++) assert(reports[i].counter==(DM_GAME_RENDER_COND ? 0 : reports[i].timestamp));
    mtl_queue_forget(0x8129); mtl_tex_release(&color);
    puts(segment ? "Vulkan segment top/bottom timestamps bracket GPU work on the guest clock" :
                   "Vulkan submission reports share a single timestamp on the guest clock");
}

// TOP and BOTTOM share the boundaries of each logical render segment. A
// target switch below the draw threshold stays inside the same segment.
static void counter_boundaries(void) {
    const char *mode=getenv("DM_NVN_GPU_TS");
    int segmented=mode && *mode ? !strcmp(mode,"segment") : DM_GAME_GPU_TS_SEGMENT;
    const char *cut_env=getenv("DM_NVN_CUT");
    int interval=cut_env ? atoi(cut_env) : 64;
    if (interval>256) return;
    uint32_t bits; replay_features(NULL,&bits);
    enum { segments=4 };
    uint32_t computed=0, sentinel=0x51ab29;
    tex_t target[2]={{.serial=0x8a20,.d={.width=64,.height=64,.depth=1,.samples=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}}};
    nvn_tex_layout(&target[0].d,&target[0].L); target[1]=target[0]; target[1].serial++;
    NVNcounterData results[segments][2]={0};
    mtl_queue_activate(target[0].serial);
    uint64_t earliest=nvn_timestamp_from_ns(guest_ns());
    mtl_sub_begin();
    for (int i=0;i<segments;i++) {
        list_t list={0};
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target[i%2]});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_range_program,.stages=3);
        RECORD(OP_SCISSOR,p_scissor,.w=64,.h=64);
        uint64_t top[]={NVN_COUNTER_TYPE_TIMESTAMP_TOP,(uintptr_t)&results[i][0]};
        uint64_t bottom[]={NVN_COUNTER_TYPE_TIMESTAMP,(uintptr_t)&results[i][1]};
        put(&list,OP_REPORT_COUNTER,top,sizeof top); put(&list,OP_END,NULL,0); mtl_exec(list.bytes);
        list=(list_t){0};
        int repeats=i==1 || i==2 ? 1 : (interval>0 ? interval : 2);
        if (i==1) {
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)&computed,.size=sizeof computed);
            push_constants(&list,1,240,&sentinel,sizeof sentinel);
            RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        } else RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        put(&list,OP_END,NULL,0);
        for (int k=0;k<repeats;k++) mtl_exec(list.bytes);
        list=(list_t){0}; put(&list,OP_REPORT_COUNTER,bottom,sizeof bottom); put(&list,OP_END,NULL,0);
        mtl_exec(list.bytes);
    }
    assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    assert(computed==sentinel);
    uint64_t latest=nvn_timestamp_from_ns(guest_ns());
    for (int i=0;i<segments;i++) {
        uint64_t a=results[i][0].timestamp, b=results[i][1].timestamp;
        assert(earliest<=a && a<=b && b<=latest);
        assert(results[i][0].counter==(DM_GAME_RENDER_COND ? 0 : a));
        assert(results[i][1].counter==(DM_GAME_RENDER_COND ? 0 : b));
        if (i && segmented && bits && ((i==1 && interval>0) || (i==3 && interval==1))) {
            assert(results[i-1][0].timestamp<a && results[i-1][1].timestamp==a && a<b);
        } else if (i) {
            assert(a==results[i-1][0].timestamp && b==results[i-1][1].timestamp);
        }
    }
    // Each successful B2T creates a boundary at the next target change even
    // with draw cuts disabled. Cross a query page, then recycle the context.
    if (segmented) {
        NVNcounterData many[259]; uint32_t pixel=0xff4080e0;
        for (int round=0;round<4;round++) {
            int count=round ? 4 : 259;
            mtl_sub_begin();
            for (int k=0;k<count;k++) {
                list_t list={0};
                uint64_t top[]={NVN_COUNTER_TYPE_TIMESTAMP_TOP,(uintptr_t)&many[k]};
                put(&list,OP_REPORT_COUNTER,top,sizeof top);
                RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&target[0],.src=(uintptr_t)&pixel,.r={.width=1,.height=1,.depth=1});
                RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target[k%2]});
                put(&list,OP_END,NULL,0); mtl_exec(list.bytes);
            }
            assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (int k=1;k<count;k++) assert(many[k-1].timestamp<=many[k].timestamp);
            if (bits) assert(many[0].timestamp<many[count-1].timestamp);
            else assert(many[0].timestamp==many[count-1].timestamp);
        }
    }
    mtl_tex_release(&target[0]); mtl_tex_release(&target[1]); mtl_queue_forget(target[0].serial);
    puts("Counter timestamps preserve GPU render-segment boundaries and guest clock bounds");
}

static void texture_clear_regions(void) {
    for (int volume = 0; volume < 2; volume++) {
        tex_t texture = {.serial=0x8119 + volume, .d={.width=8,.height=8,.depth=4,.levels=2,
            .target=volume ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D_ARRAY, .format=NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&texture.d, &texture.L);
        list_t list = {0};
        uint8_t pixels[2][4*8*8*4] = {{0}};
        for (int lv=0; lv<2; lv++) {
            int size = 8 >> lv, layers = volume ? 4 >> lv : 4;
            RECORD(OP_CLEAR_TEX, p_clear_tex, .tex=&texture, .has_view=1, .view={.set_levels=1,.base_level=lv,.nlevels=1},
                   .r={.width=size,.height=size,.depth=layers}, .mask=15, .c.f={1,0,1,1});
        }
        // ClearTexture has its own (clipped) region and ignores the current
        // draw scissor. Unsigned values are interpreted in the selected view.
        RECORD(OP_SCISSOR, p_scissor, .x=7,.y=7,.w=1,.h=1);
        int first = volume ? 1 : 2, count = volume ? 1 : 2;
        view_t alias = {.set_levels=1,.base_level=1,.nlevels=1, .set_layers=!volume,.min_layer=volume?0:1,
                        .nlayers=volume?0:3, .set_format=1,.format=NVN_FORMAT_RGBA8UI};
        RECORD(OP_CLEAR_TEX, p_clear_tex, .tex=&texture, .has_view=1,.view=alias,.is_uint=1,
               .r={.xoffset=-1,.yoffset=1,.zoffset=1,.width=4,.height=2,.depth=count}, .mask=6,.c.u={9,39,71,0});
        for (int lv=0; lv<2; lv++) {
            int size = 8 >> lv, layers = volume ? 4 >> lv : 4;
            RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&texture, .has_view=1, .view={.set_levels=1,.base_level=lv,.nlevels=1},
                   .r={.width=size,.height=size,.depth=layers}, .src=(uintptr_t)pixels[lv]);
        }
        put(&list, OP_END, NULL, 0); mtl_queue_activate(0x8119);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int lv=0; lv<2; lv++) {
            int size = 8 >> lv, layers = volume ? 4 >> lv : 4;
            for (int z=0; z<layers; z++) for (int y=0; y<size; y++) for (int x=0; x<size; x++) {
                int hit = lv == 1 && z >= first && z < first+count && x < 3 && y >= 1 && y < 3;
                uint8_t expected[] = {255, hit ? 39 : 0, hit ? 71 : 255, 255};
                if (memcmp(expected, &pixels[lv][4*((z*size+y)*size+x)], 4))
                    die("texture clear volume %d level %d slice %d at %d,%d", volume,lv,z,x,y);
            }
        }
        mtl_tex_release(&texture); mtl_queue_forget(0x8119);
    }
    puts("Vulkan texture clears preserve neighboring channels, pixels, array layers, volume slices and mip levels");
}

static void integer_attachment_clears(void) {
    tex_t target = {.serial = 0x8114, .d = {.width = 4, .height = 2, .depth = 1, .levels = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA32F}};
    nvn_tex_layout(&target.d, &target.L);
    for (int signed_view = 0; signed_view < 2; signed_view++) {
        view_t view = {.set_format = 1, .format = signed_view ? NVN_FORMAT_RGBA32I : NVN_FORMAT_RGBA32UI};
        uint32_t pixels[2][4][4];
        list_t list = {0};
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&target}, .has_cview = {1}, .cview = {view});
        RECORD(OP_SCISSOR, p_scissor, .w = 4, .h = 2);
        RECORD(OP_CLEAR_COLOR, p_clear_color, .mask = 15, .rgba = {1,2,3,4});
        RECORD(OP_SCISSOR, p_scissor, .x = 1, .w = 2, .h = 2);
        RECORD(OP_CLEAR_COLOR, p_clear_color, .mask = 15, .rgba = {5,-6,7,8});
        RECORD(OP_SCISSOR, p_scissor, .x = 2, .w = 2, .h = 2);
        RECORD(OP_CLEAR_COLOR, p_clear_color, .mask = 5, .rgba = {4294967296.f,9,-17.5f,9});
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &target, .src = (uintptr_t)pixels,
               .r = {.width=4,.height=2,.depth=1});
        put(&list, OP_END, NULL, 0);
        mtl_queue_activate(0x8114);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int y = 0; y < 2; y++) for (int x = 0; x < 4; x++) {
            int middle = x == 1 || x == 2;
            uint32_t expected[] = {middle ? 5u : 1u, middle ? (signed_view ? (uint32_t)-6 : 0) : 2,
                                   middle ? 7u : 3u, middle ? 8u : 4u};
            if (x >= 2) { expected[0] = signed_view ? INT32_MAX : UINT32_MAX; expected[2] = signed_view ? (uint32_t)-17 : 0; }
            if (memcmp(pixels[y][x], expected, sizeof expected)) die("integer attachment clear (signed %d) at %d,%d", signed_view,x,y);
        }
        mtl_queue_forget(0x8114);
    }
    mtl_tex_release(&target);
    puts("Vulkan load, rectangular and masked clears use the signed or unsigned attachment view");
}

static void scissored_attachment_clears(void) {
    tex_t color[2], depth, sampled;
    tex_t target = {.d = {.width = 8, .height = 8, .depth = 1, .levels = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&target.d, &target.L);
    color[0] = color[1] = sampled = depth = target;
    color[0].serial = 0x8110; color[1].serial = 0x8111;
    depth.serial = 0x8112; sampled.serial = 0x8113;
    depth.d.format = NVN_FORMAT_DEPTH32F;
    nvn_tex_layout(&depth.d, &depth.L);
    uint8_t pixels[3][8*8*4];
    list_t list = {0};
    RECORD(OP_SET_RT, p_set_rt, .n = 2, .color = {color, color+1}, .depth = &depth);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .index = 0, .mask = 15, .rgba = {0,0,1,1});
    RECORD(OP_CLEAR_COLOR, p_clear_color, .index = 1, .mask = 15, .rgba = {1,0,1,1});
    RECORD(OP_CLEAR_DS, p_clear_ds, .depth = 1, .depth_mask = 1);
    RECORD(OP_SCISSOR, p_scissor, .x = -1, .y = 1, .w = 4, .h = 3);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .index = 1, .mask = 2, .rgba = {0,1,0,0});
    RECORD(OP_CLEAR_DS, p_clear_ds, .depth = 0.25f, .depth_mask = 1);
    RECORD(OP_SCISSOR, p_scissor, .x = 5, .y = 3, .w = INT32_MAX, .h = 2);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .index = 0, .mask = 15, .rgba = {1,0,0,1});
    RECORD(OP_CLEAR_DS, p_clear_ds, .depth = 0.75f, .depth_mask = 1);
    RECORD(OP_SCISSOR, p_scissor, .x = 12, .y = 0, .w = 2, .h = 8);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .index = 1, .mask = 15, .rgba = {0,0,0,0});
    RECORD(OP_CLEAR_DS, p_clear_ds, .depth = 0, .depth_mask = 1);
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &depth_range_program, .stages = 3);
    RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
    RECORD(OP_SCISSOR, p_scissor, .x = 1, .y = 2, .w = 5, .h = 1);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .index = 0, .mask = 8, .rgba = {1,1,0,0});
    RECORD(OP_SCISSOR, p_scissor, .x = 1, .y = 2, .w = 0, .h = 3);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .index = 0, .mask = 15, .rgba = {0,0,0,0});
    RECORD(OP_CLEAR_DS, p_clear_ds, .depth = 0, .depth_mask = 1);
    RECORD(OP_SCISSOR, p_scissor, .x = 0, .y = 0, .w = 8, .h = 8);
    RECORD(OP_CLEAR_COLOR, p_clear_color, .index = 0, .mask = 0, .rgba = {0,0,0,0});
    for (int i = 0; i < 2; ++i) {
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &color[i], .src = (uintptr_t)pixels[i],
               .r = {.width = 8, .height = 8, .depth = 1});
    }
    samp_par nearest = {0};
    mtl_reg_tex(907, &depth, NULL); mtl_reg_samp(907, &nearest);
    RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&sampled});
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &depth_copy_program, .stages = 3);
    RECORD(OP_BIND_TEX1, p_bind_tex1, .stage = 1|BIND_TEX1_NOBASE, .slot = 0, .h = 907|(907ull<<20));
    RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
    RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &sampled, .src = (uintptr_t)pixels[2],
           .r = {.width = 8, .height = 8, .depth = 1});
    put(&list, OP_END, NULL, 0);
    for (int origin = 0; origin < 2; ++origin) {
        mtl_queue_activate(0x8110);
        mtl_set_origin_lower_left(origin); mtl_set_queue_origin_lower_left(origin);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            int left = x < 3 && y >= 1 && y < 4;
            int right = x >= 5 && y >= 3 && y < 5;
            uint8_t expected[][4] = {{right?255:0,0,right?0:255,y==2&&x>=1&&x<6?0:255}, {255,left?255:0,255,255},
                                     {left?64:right?191:255,0,0,255}};
            for (int i = 0; i < 3; ++i) if (memcmp(pixels[i] + 4*(8*y+x), expected[i], 4))
                die("scissored clear origin %d attachment %d at %d,%d", origin, i, x, y);
        }
        mtl_queue_forget(0x8110);
    }
    mtl_reg_tex(907, NULL, NULL);
    mtl_tex_release(&color[0]); mtl_tex_release(&color[1]); mtl_tex_release(&depth); mtl_tex_release(&sampled);
    puts("Vulkan clipped clears preserved both color targets and sampled depth");
}

// Same expectations as Metal's t_queue_origin: queue-latched raster origin
// changes facing; the device origin changes clip Y within the viewport only.
static void origin_facing(void) {
    tex_t output = {.serial=0x8200, .d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    const int front_in_clip[2][2][2] = {{{0,1},{1,0}},{{1,0},{0,1}}};
    for (int device=0;device<2;device++) for (int queue=0;queue<2;queue++)
    for (int winding=0;winding<2;winding++) for (int cull=0;cull<4;cull++) for (int ccw=0;ccw<2;ccw++) for (int flip=0;flip<2;flip++) {
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
        RECORD(OP_VP_SWIZZLE,p_vp_swizzle,.n=1,.sw={{0,flip,0,0}});
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
        put(&list,OP_END,NULL,0);
        mtl_set_origin_lower_left(device); mtl_queue_activate(120+queue); mtl_set_queue_origin_lower_left(queue);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        int front=ccw==(front_in_clip[queue][device][winding]^flip);
        int visible=cull==NVN_FACE_NONE || (cull==NVN_FACE_FRONT && !front) || (cull==NVN_FACE_BACK && front);
        uint8_t want[]={visible&&!front?255:0,visible&&front?255:0,visible?0:255,255};
        for (int i=0;i<64;i++) { if(memcmp(pixels+4*i,want,4)) fprintf(stderr,"origin device%d queue%d winding%d cull%d ccw%d flip%d got%d,%d,%d expected%d,%d,%d\n",device,queue,winding,cull,ccw,flip,pixels[4*i],pixels[4*i+1],pixels[4*i+2],want[0],want[1],want[2]); assert(!memcmp(pixels+4*i,want,4)); }
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
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,1,4,6}});
        // An unsupported negative-X selector uses Metal's identity fallback.
        RECORD(OP_VP_SWIZZLE,p_vp_swizzle,.n=1,.sw={{device,0,0,0}});
        // Updating an inactive slot must leave the left viewport unchanged.
        RECORD(OP_VIEWPORTS,p_viewports,.first=1,.n=1,.v={{3,0,1,1}});
        RECORD(OP_VP_SWIZZLE,p_vp_swizzle,.first=1,.n=1,.sw={{0,1,0,0}});
        RECORD(OP_SCISSOR,p_scissor,.x=1,.y=2,.w=6,.h=3);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=6,.instances=1);
        // Move viewport zero to the right; its negative-Y swizzle flips
        // both clip-space coverage and facing, but never the memory scissor.
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{4,1,4,6}});
        RECORD(OP_VP_SWIZZLE,p_vp_swizzle,.n=1,.sw={{0,1,0,0}});
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=6,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
        put(&list,OP_END,NULL,0);
        mtl_set_origin_lower_left(device); mtl_queue_activate(122+device); mtl_set_queue_origin_lower_left(device);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(int y=0;y<8;y++) for(int x=0;x<8;x++) {
            int flipped = x >= 4;
            int drawn = x >= 1 && x < 7 && ((device ^ flipped) ? (y == 2 || y == 3) : y == 4);
            uint8_t want[]={drawn && flipped ? 255 : 0, drawn && !flipped ? 255 : 0, drawn ? 0 : 255, 255}; assert(!memcmp(pixels+4*(8*y+x),want,4));
        }
        mtl_queue_forget(122+device);
    }
    mtl_queue_forget(120); mtl_queue_forget(121); mtl_set_origin_lower_left(0);
    puts("Vulkan queue/device origins preserve facing with both viewport swizzles (256 cases) and independent viewport slots");
}

static void texture_copy_regions(void) {
    for (int mode = 0; mode < 6; mode++) {
        int volume = mode == 0, convert = mode >= 2;
        int flags = (mode == 2 || mode == 4 ? NVN_COPY_FLAGS_MIRROR_X_BIT : 0) |
                    (mode == 3 || mode == 4 ? NVN_COPY_FLAGS_MIRROR_Y_BIT : 0);
        tex_t src = {.serial=0x8150+2*mode, .d={.width=8,.height=8,.depth=8,.levels=2,
                     .target=volume ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_RGBA8}};
        tex_t dst = src; dst.serial++; if (convert) dst.d.format=NVN_FORMAT_RG8;
        nvn_tex_layout(&src.d,&src.L); nvn_tex_layout(&dst.d,&dst.L);
        int slices = volume ? 4 : 8, channels = convert ? 2 : 4;
        uint8_t source[8][4][4][4], result[sizeof source];
        for (int z=0;z<slices;z++) for (int y=0;y<4;y++) for (int x=0;x<4;x++)
            for (int ch=0;ch<4;ch++) source[z][y][x][ch]=1+25*z+4*y+x+ch;
        view_t mip = {.set_levels=1,.base_level=1,.nlevels=1};
        view_t sv = mip, dv = mip;
        if (!volume) { sv.set_layers=dv.set_layers=1; sv.min_layer=2; dv.min_layer=1; sv.nlayers=dv.nlayers=4; }
        list_t list={0};
        NVNcopyRegion whole={.width=4,.height=4,.depth=slices};
        RECORD(OP_COPY_B2T,p_copy_b2t,.src=(uintptr_t)source,.tex=&src,.view=mip,.has_view=1,.r=whole);
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&dst,.view=mip,.has_view=1,.r=whole,.mask=15,.c.f={1,1,1,1});
        RECORD(OP_COPY_T2T,p_copy_t2t,.src=&src,.dst=&dst,.has_sv=1,.sv=sv,.has_dv=1,.dv=dv,.flags=flags,
               .sr={.xoffset=1,.zoffset=1,.width=2,.height=2,.depth=2},
               .dr={.xoffset=1,.yoffset=1,.zoffset=1,.width=2,.height=2,.depth=2});
        RECORD(OP_COPY_T2B,p_copy_b2t,.src=(uintptr_t)result,.tex=&dst,.view=mip,.has_view=1,.r=whole);
        put(&list,OP_END,NULL,0); mtl_queue_activate(0x8150);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (int z=0;z<slices;z++) for (int y=0;y<4;y++) for (int x=0;x<4;x++) for (int ch=0;ch<channels;ch++) {
            int dz=z-(volume ? 1 : 2), want=255;
            if (dz>=0 && dz<2 && x>=1 && x<3 && y>=1 && y<3) {
                int sx=(flags & NVN_COPY_FLAGS_MIRROR_X_BIT) ? 3-x : x;
                int sy=(flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) ? 2-y : y-1;
                want=source[(volume ? 1 : 3)+dz][sy][sx][ch];
            }
            int got=result[((z*4+y)*4+x)*channels+ch];
            if (got!=want) die("texture copy mode %d slice %d at %d,%d channel %d: %d, expected %d",mode,z,x,y,ch,got,want);
        }
        mtl_tex_release(&src); mtl_tex_release(&dst); mtl_queue_forget(0x8150);
    }
    puts("Vulkan texture copies preserve 3D slices and array layers, convert RGBA to RG and mirror both axes");
}

// R11G11B10F is renderable on the Android test driver but lacks BLIT_DST.
// Copy two source layers into a subrectangle of a nonzero mip; the blue
// channel distinguishes layers and the untouched green border guards writes.
static void hdr_scaled_copies(void) {
    tex_t src={.serial=0x8fbe00,.d={.width=2,.height=2,.depth=2,.levels=1,.target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_R11G11B10F}};
    tex_t dst=src; dst.serial++; dst.d.width=dst.d.height=12; dst.d.depth=4; dst.d.levels=2;
    nvn_tex_layout(&src.d,&src.L); nvn_tex_layout(&dst.d,&dst.L);
    uint32_t pixels[2][2][2], got[2][4][6][6], initial[4][6][6];
    const uint32_t green=0x3c0u<<11;
    for (int z=0; z<2; ++z) for (int y=0; y<2; ++y) for (int x=0; x<2; ++x)
        pixels[z][y][x]=(x?0x3c0u:0) | (y?green:0) | ((z?0x1c0u:0x1e0u)<<22);
    for (int z=0; z<4; ++z) for (int y=0; y<6; ++y) for (int x=0; x<6; ++x) initial[z][y][x]=green;
    view_t mip={.set_levels=1,.base_level=1,.nlevels=1}, slice=mip; slice.set_layers=1; slice.min_layer=1; slice.nlayers=2;
    list_t list={0};
    RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&src,.src=(uintptr_t)pixels,.r={.width=2,.height=2,.depth=2});
    for (int linear=0; linear<2; ++linear) {
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&dst,.has_view=1,.view=mip,.src=(uintptr_t)initial,.r={.width=6,.height=6,.depth=4});
        RECORD(OP_COPY_T2T,p_copy_t2t,.src=&src,.dst=&dst,.has_dv=1,.dv=slice,
               .flags=NVN_COPY_FLAGS_MIRROR_X_BIT|NVN_COPY_FLAGS_MIRROR_Y_BIT|(linear?NVN_COPY_FLAGS_LINEAR_FILTER_BIT:0),
               .sr={.width=2,.height=2,.depth=2},.dr={.xoffset=1,.yoffset=1,.width=4,.height=4,.depth=2});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&dst,.has_view=1,.view=mip,.src=(uintptr_t)got[linear],.r={.width=6,.height=6,.depth=4});
    }
    put(&list,OP_END,NULL,0); mtl_queue_activate(0x8fbe00);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    const uint32_t levels[2][4]={{0x3c0,0x3c0,0,0},{0x3c0,0x3a0,0x340,0}};
    for (int linear=0; linear<2; ++linear) for (int z=0; z<4; ++z) for (int y=0; y<6; ++y) for (int x=0; x<6; ++x) {
        uint32_t want=green;
        if ((z==1 || z==2) && x>=1 && x<=4 && y>=1 && y<=4)
            want=levels[linear][x-1] | (levels[linear][y-1]<<11) | ((z==1?0x1e0u:0x1c0u)<<22);
        if (got[linear][z][y][x]!=want) die("HDR %s layer %d (%d,%d) returned %#x expected %#x",linear?"linear":"nearest",z,x,y,got[linear][z][y][x],want);
    }
    mtl_tex_release(&src); mtl_tex_release(&dst); mtl_queue_forget(0x8fbe00);
    puts("Vulkan packed HDR nearest and bilinear copies preserved mip, layer and border pixels");
}

static void stencil_faces(void) {
    tex_t packed={.serial=0x8170,.d={.width=4,.height=2,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_DEPTH32F_STENCIL8}};
    tex_t color=packed; color.serial++; color.d.format=NVN_FORMAT_R32F;
    nvn_tex_layout(&packed.d,&packed.L); nvn_tex_layout(&color.d,&color.L);
    float vertices[][2]={{-1,-1},{3,-1},{-1,3},{-1,-1},{-1,3},{3,-1}};
    list_t list={0};
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color},.depth=&packed);
    RECORD(OP_CLEAR_DS,p_clear_ds,.stencil=0xa5,.stencil_mask=255);
    RECORD(OP_SCISSOR,p_scissor,.x=1,.w=2,.h=2);
    RECORD(OP_CLEAR_DS,p_clear_ds,.stencil=0x99,.stencil_mask=255);
    p_bind_state polygon={.kind=4}, state={.kind=5};
    poly_st poly={.front=NVN_FRONT_FACE_CCW};
    memcpy(polygon.bytes,&poly,sizeof poly); put(&list,OP_BIND_STATE,&polygon,sizeof polygon);
    ds_st ds={.st=1,.sf_f=NVN_COMPARE_FUNC_EQUAL,.sf_b=NVN_COMPARE_FUNC_EQUAL,.fail_f=NVN_STENCIL_OP_KEEP,
              .fail_b=NVN_STENCIL_OP_KEEP,.zfail_f=NVN_STENCIL_OP_KEEP,.zfail_b=NVN_STENCIL_OP_KEEP,
              .zpass_f=NVN_STENCIL_OP_REPLACE,.zpass_b=NVN_STENCIL_OP_REPLACE};
    memcpy(state.bytes,&ds,sizeof ds); put(&list,OP_BIND_STATE,&state,sizeof state);
    RECORD(OP_STENCIL,p_stencil,.face=NVN_FACE_FRONT,.which=0,.value_mask=0xa0);
    RECORD(OP_STENCIL,p_stencil,.face=NVN_FACE_BACK,.which=0,.value_mask=0x05);
    RECORD(OP_STENCIL,p_stencil,.face=NVN_FACE_FRONT,.which=1,.mask=0x0f);
    RECORD(OP_STENCIL,p_stencil,.face=NVN_FACE_BACK,.which=1,.mask=0xf0);
    RECORD(OP_STENCIL,p_stencil,.face=NVN_FACE_FRONT,.which=2,.ref=0xec);
    RECORD(OP_STENCIL,p_stencil,.face=NVN_FACE_BACK,.which=2,.ref=0x37);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&origin_program,.stages=3);
    va_st va={.fmt=NVN_FORMAT_RG32F}; p_bind_vattr attr={.n=1}; memcpy(attr.st,&va,sizeof va); put(&list,OP_BIND_VATTR,&attr,sizeof attr);
    RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=1,.st={{.stride=8}});
    RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=sizeof vertices,.addr=(uintptr_t)vertices);
    for (int face=0;face<2;face++) {
        RECORD(OP_SCISSOR,p_scissor,.x=2*face,.w=2,.h=2);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.first=3*face,.count=3,.instances=1);
    }
    const int want[]={0xac,0x99,0x99,0x35};
    float pixels[2][2][4];
    RECORD(OP_STENCIL,p_stencil,.face=3,.which=0,.value_mask=255);
    RECORD(OP_STENCIL,p_stencil,.face=3,.which=1,.mask=0);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_range_program,.stages=3);
    for (int mismatch=0;mismatch<2;mismatch++) {
        RECORD(OP_SCISSOR,p_scissor,.w=4,.h=2);
        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0});
        for (int x=0;x<4;x++) {
            RECORD(OP_SCISSOR,p_scissor,.x=x,.w=1,.h=2);
            RECORD(OP_STENCIL,p_stencil,.face=3,.which=2,.ref=want[x]^(mismatch ? 0x40 : 0));
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        }
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)pixels[mismatch],.r={.width=4,.height=2,.depth=1});
    }
    put(&list,OP_END,NULL,0); mtl_set_origin_lower_left(0); mtl_queue_activate(0x8170); mtl_set_queue_origin_lower_left(0);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int y=0;y<2;y++) for (int x=0;x<4;x++)
        if (pixels[0][y][x]!=.25f || pixels[1][y][x]!=0) die("stencil face/mask mismatch at %d,%d",x,y);
    mtl_tex_release(&packed); mtl_tex_release(&color); mtl_queue_forget(0x8170);
    puts("Vulkan front and back stencil comparisons, write masks and references preserve all four canaries");
}

static void blend_constants(void) {
    tex_t target[2]={{.serial=0x8172,.d={.width=3,.height=1,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}}};
    target[1]=target[0]; target[1].serial++;
    nvn_tex_layout(&target[0].d,&target[0].L); nvn_tex_layout(&target[1].d,&target[1].L);
    const float seed[2][4]={{0,.25f,.75f,.5f},{1,.75f,.25f,1}}, fragment[2][4]={{.75f,.5f,.25f,1},{.25f,.5f,.75f,0}};
    const p_blend_color constants[]={{{.125f,.25f,.5f,.75f}},{{1,.75f,.5f,.25f}}};
    uint8_t image[2][3][4];
    list_t list={0}; RECORD(OP_SET_RT,p_set_rt,.n=2,.color={&target[0],&target[1]});
    for (int i=0;i<2;i++) {
        p_clear_color clear={.index=i,.mask=15}; memcpy(clear.rgba,seed[i],sizeof clear.rgba);
        put(&list,OP_CLEAR_COLOR,&clear,sizeof clear);
        blend_st blend={.target=i,.src_rgb=i ? NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA : NVN_BLEND_FUNC_CONSTANT_COLOR,
                        .dst_rgb=i ? NVN_BLEND_FUNC_CONSTANT_ALPHA : NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR,
                        .src_a=i ? NVN_BLEND_FUNC_ZERO : NVN_BLEND_FUNC_CONSTANT_ALPHA,
                        .dst_a=i ? NVN_BLEND_FUNC_ONE : NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA,
                        .eq_rgb=NVN_BLEND_EQUATION_ADD,.eq_a=NVN_BLEND_EQUATION_ADD};
        p_bind_state state={.kind=0}; memcpy(state.bytes,&blend,sizeof blend); put(&list,OP_BIND_STATE,&state,sizeof state);
    }
    color_st enable={.blend_en=3,.logic_op=NVN_LOGIC_OP_COPY,.alpha_func=NVN_ALPHA_FUNC_ALWAYS};
    p_bind_state state={.kind=1}; memcpy(state.bytes,&enable,sizeof enable); put(&list,OP_BIND_STATE,&state,sizeof state);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&blend_mrt_program,.stages=3);
    for (int i=0;i<2;i++) {
        put(&list,OP_BLEND_COLOR,&constants[i],sizeof constants[i]);
        RECORD(OP_SCISSOR,p_scissor,.x=2*i,.w=1,.h=1);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    }
    for (int i=0;i<2;i++)
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target[i],.r={.width=3,.height=1,.depth=1},.src=(uintptr_t)image[i]);
    put(&list,OP_END,NULL,0); mtl_queue_activate(0x8172);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int i=0;i<2;i++) for (int x=0;x<3;x++) for (int ch=0;ch<4;ch++) {
        const float *c=constants[x/2].rgba;
        float factor=x==1 ? 0 : i==0 ? c[ch] : ch==3 ? 0 : 1-c[3];
        int want=(int)(255*(factor*fragment[i][ch]+(1-factor)*seed[i][ch])+.5f);
        if (abs(image[i][x][ch]-want)>1) die("blend constant target %d pixel %d channel %d: %u, expected %d",i,x,ch,image[i][x][ch],want);
    }
    mtl_tex_release(&target[0]); mtl_tex_release(&target[1]); mtl_queue_forget(0x8172);
    puts("Vulkan blend constants update across draws and respect two independent target equations");
}

static void instance_divisors(void) {
    tex_t color={.serial=0x8175,.d={.width=7,.height=1,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&color.d,&color.L);
    uint8_t red[12][3], green[12][2], gpu_green[sizeof green];
    for (int i=0;i<12;i++) { memset(red[i],13*i+1,sizeof red[i]); memset(green[i],17*i+3,sizeof green[i]); }
    uint16_t indices[]={0,1,2,3,4,5};
    mtl_queue_activate(0x8175);
    for (int indexed=0;indexed<2;indexed++) for (int div=1;div<=3;div++) {
        memset(gpu_green,0,sizeof gpu_green);
        uint8_t pixels[7][4];
        list_t list={0};
        uint64_t copy[]={(uintptr_t)green,(uintptr_t)gpu_green,sizeof green};
        put(&list,OP_COPY_B2B,copy,sizeof copy);
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color});
        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0,0,1,1});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&divisor_program,.stages=3);
        p_bind_vattr attr={.n=2}; va_st va[]={{.fmt=NVN_FORMAT_R8},{.fmt=NVN_FORMAT_R8,.stream=1}};
        memcpy(attr.st,va,sizeof va); put(&list,OP_BIND_VATTR,&attr,sizeof attr);
        RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=2,.st={{.stride=3,.divisor=div},{.stride=2,.divisor=3}});
        RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=sizeof red,.addr=(uintptr_t)red);
        RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=1,.size=sizeof green,.addr=(uintptr_t)gpu_green);
        if (indexed) {
            RECORD(OP_DRAW_IDX,p_draw_idx,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.itype=1,.count=6,.instances=5,.base_instance=5,.ibuf=(uintptr_t)indices);
        } else {
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=6,.instances=5,.base_instance=5);
        }
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.r={.width=7,.height=1,.depth=1},.src=(uintptr_t)pixels);
        put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (int x=0;x<7;x++) {
            uint8_t want[]={0,0,255,255};
            if (x>0 && x<6) { want[0]=red[5+(x-1)/div][0]; want[1]=green[5+(x-1)/3][0]; want[2]=0; }
            if (memcmp(pixels[x],want,4)) die("instance divisor %d indexed %d pixel %d got %u,%u wanted %u,%u",div,indexed,x,pixels[x][0],pixels[x][1],want[0],want[1]);
        }
    }
    mtl_tex_release(&color); mtl_queue_forget(0x8175);
    puts("Vulkan vertex streams use base instance plus divided instance offset for direct and indexed draws");
}

static void downsample_view(void) {
    tex_t src = {.serial=0x8160,.d={.width=8,.height=8,.depth=2,.levels=2,.target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_RGBA8}};
    tex_t dst = src; dst.serial++; dst.d.width=dst.d.height=4; dst.d.format=NVN_FORMAT_RG8;
    nvn_tex_layout(&src.d,&src.L); nvn_tex_layout(&dst.d,&dst.L);
    view_t view = {.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=1,.nlayers=1};
    uint8_t input[4][4][4], pixels[2][2][2][2];
    for (int y=0;y<4;y++) for (int x=0;x<4;x++)
        for (int ch=0;ch<4;ch++) input[y][x][ch]=16*x+32*y+3*ch;
    list_t list={0};
    RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&dst,.has_view=1,.view={.set_levels=1,.base_level=1,.nlevels=1},
           .r={.width=2,.height=2,.depth=2},.mask=15,.c.f={1,1,1,1});
    RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&src,.has_view=1,.view=view,.src=(uintptr_t)input,.r={.width=4,.height=4,.depth=1});
    // The public DownsampleTextureView recorder writes the base dimensions.
    RECORD(OP_COPY_T2T,p_copy_t2t,.src=&src,.dst=&dst,.flags=DM_COPY_RESOLVE,.has_sv=1,.sv=view,.has_dv=1,.dv=view,
           .sr={.width=8,.height=8,.depth=1},.dr={.width=4,.height=4,.depth=1});
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&dst,.has_view=1,.view={.set_levels=1,.base_level=1,.nlevels=1},
           .r={.width=2,.height=2,.depth=2},.src=(uintptr_t)pixels);
    put(&list,OP_END,NULL,0); mtl_queue_activate(0x8160);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int z=0;z<2;z++) for (int y=0;y<2;y++) for (int x=0;x<2;x++) for (int ch=0;ch<2;ch++) {
        int want=z ? 24+32*x+64*y+3*ch : 255;
        if (pixels[z][y][x][ch]!=want) die("downsample view layer %d at %d,%d got %u, expected %d",z,x,y,pixels[z][y][x][ch],want);
    }
    mtl_tex_release(&src); mtl_tex_release(&dst); mtl_queue_forget(0x8160);
}

static void depth_copy_filter(void) {
    tex_t src={.serial=0x8162,.d={.width=5,.height=1,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_DEPTH32F}};
    tex_t dst=src; dst.serial++; dst.d.width=3;
    nvn_tex_layout(&src.d,&src.L); nvn_tex_layout(&dst.d,&dst.L);
    float input[]={.125f,.25f,.625f,.875f,1}, pixels[3]={0};
    list_t list={0};
    RECORD(OP_COPY_B2T,p_copy_b2t,.src=(uintptr_t)input,.tex=&src,.r={.width=5,.height=1,.depth=1});
    RECORD(OP_COPY_T2T,p_copy_t2t,.src=&src,.dst=&dst,.flags=NVN_COPY_FLAGS_LINEAR_FILTER_BIT|NVN_COPY_FLAGS_MIRROR_X_BIT,
           .sr={.width=5,.height=1,.depth=1},.dr={.width=3,.height=1,.depth=1});
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&dst,.src=(uintptr_t)pixels,.r={.width=3,.height=1,.depth=1});
    put(&list,OP_END,NULL,0); mtl_queue_activate(0x8162);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    // Metal's depth copy uses an unfiltered read at the mapped texel even
    // when color copies would filter. Bilinear interpolation differs at both ends.
    for (int i=0;i<3;i++) if (pixels[i]!=input[4-2*i])
        die("depth copy with the linear flag: pixel %d got %g, expected %g",i,pixels[i],input[4-2*i]);
    mtl_tex_release(&src); mtl_tex_release(&dst); mtl_queue_forget(0x8162);
}

static void depth_plane_copy(void) {
    tex_t src={.serial=0x8164,.d={.width=4,.height=2,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_DEPTH32F}};
    tex_t packed=src; packed.serial++; packed.d.format=NVN_FORMAT_DEPTH32F_STENCIL8;
    tex_t dst=src; dst.serial+=2;
    tex_t color=src; color.serial+=3; color.d.format=NVN_FORMAT_R32F;
    nvn_tex_layout(&src.d,&src.L); nvn_tex_layout(&dst.d,&dst.L);
    nvn_tex_layout(&packed.d,&packed.L); nvn_tex_layout(&color.d,&color.L);
    float source[2][4]={{.0625f,.125f,.1875f,.25f},{.3125f,.375f,.4375f,.5f}}, result[2][4], stencil[2][4];
    NVNcopyRegion all={.width=4,.height=2,.depth=1};
    p_clear_tex clear={.tex=&packed,.r=all,.mask=15}; clear.c.f[0]=.875f; clear.c.u[1]=0x37;
    list_t list={0}; put(&list,OP_CLEAR_TEX,&clear,sizeof clear);
    RECORD(OP_COPY_B2T,p_copy_b2t,.src=(uintptr_t)source,.tex=&src,.r=all);
    RECORD(OP_COPY_T2T,p_copy_t2t,.src=&src,.dst=&packed,.sr={.xoffset=1,.yoffset=1,.width=2,.height=1,.depth=1},
           .dr={.xoffset=1,.width=2,.height=1,.depth=1});
    RECORD(OP_COPY_T2T,p_copy_t2t,.src=&packed,.dst=&dst,.sr=all,.dr=all);
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&dst,.src=(uintptr_t)result,.r=all);
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color},.depth=&packed);
    RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0});
    p_bind_state state={.kind=5};
    ds_st ds={.st=1,.sf_f=NVN_COMPARE_FUNC_EQUAL,.sf_b=NVN_COMPARE_FUNC_EQUAL};
    memcpy(state.bytes,&ds,sizeof ds); put(&list,OP_BIND_STATE,&state,sizeof state);
    RECORD(OP_STENCIL,p_stencil,.face=3,.which=2,.ref=0x37);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_range_program,.stages=3);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)stencil,.r=all);
    put(&list,OP_END,NULL,0); mtl_queue_activate(0x8164);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int y=0;y<2;y++) for (int x=0;x<4;x++) {
        float want=y==0 && (x==1 || x==2) ? source[1][x] : .875f;
        if (result[y][x]!=want || stencil[y][x]==0)
            die("D32 depth plane at %d,%d got %g, expected %g, stencil probe %g",x,y,result[y][x],want,stencil[y][x]);
    }
    mtl_tex_release(&src); mtl_tex_release(&packed); mtl_tex_release(&dst); mtl_tex_release(&color); mtl_queue_forget(0x8164);
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
            // The untouched high nibble participates in the compare. Alternate
            // depth writes on the same packed mip/layer and sample the result.
            list = (list_t){0};
            RECORD(OP_CLEAR_DS, p_clear_ds, .depth = .75f, .depth_mask = 1, .stencil = 0xaa, .stencil_mask = 0xff);
            RECORD(OP_SCISSOR, p_scissor, .x = 2, .y = 1, .w = 3, .h = 4);
            RECORD(OP_CLEAR_DS, p_clear_ds, .depth = .25f, .depth_mask = scale, .stencil = 0x35, .stencil_mask = 0x0f);
            RECORD(OP_SCISSOR, p_scissor, .w = 8, .h = 8);
            uint8_t observed[3][256];
            for (int i = 0; i < 2; i++) {
                RECORD(OP_CLEAR_COLOR, p_clear_color, .mask = 15, .rgba = {0,0,1,1});
                RECORD(OP_STENCIL, p_stencil, .face = 3, .which = 2, .ref = i ? 0xaa : 0xa5);
                RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
                RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)observed[i],
                       .r = {.width=8,.height=8,.depth=1});
            }
            RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
            RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &depth_copy_program, .stages = 3);
            RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
            RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)observed[2],
                   .r = {.width=8,.height=8,.depth=1});
            put(&list, OP_END, NULL, 0);
            mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
                int inside = x >= 2 && x < 5 && y >= 1 && y < 5, at = 4*(8*y+x);
                if (observed[0][at+1] != (inside ? 255 : 0) || observed[1][at+1] != (inside ? 0 : 255)
                    || abs(observed[2][at] - (inside && scale ? 64 : 191)) > 1)
                    die("masked DS format %#x depth write %d at %d,%d: stencil passes %u/%u, depth %u",
                        formats[k], scale, x, y, observed[0][at+1], observed[1][at+1], observed[2][at]);
            }
        }
        mtl_queue_forget(901); mtl_reg_tex(900,NULL,NULL); mtl_tex_release(&source);mtl_tex_release(&target);mtl_tex_release(&output);
    }
    puts("Depth and stencil copies preserve subrectangles, mip levels and array layers at equal and scaled sizes");
}

static void draw_parameters(void) {
    tex_t output={.serial=0x3900,.d={.width=8,.height=8,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d,&output.L);
    for (int mode=0;mode<6;++mode) {
        list_t list={0}; uint8_t pixels[8*8*4]={0};
        const int32_t bases[]={7,17,0,0x101,0x40110010,0};
        int32_t base_vertex=mode==1 ? 2 : mode==2 ? -2 : 0;
        int32_t expected[4]={bases[mode],mode ? base_vertex : 5,mode<=1 ? 5 : 0,0};
        uint16_t first_index=mode==1 ? 3 : mode==2 ? 2 : 0;
        uint16_t indices[3]={first_index,first_index+1,first_index+2};
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&output,.r={.width=8,.height=8,.depth=1},.mask=15,.c.f={0,0,1,1});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&draw_parameters_program,.stages=3);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=0,.index=0,.addr=(uintptr_t)expected,.size=sizeof expected);
        if (mode) {
            struct { uint64_t address, size; uint8_t bytes[sizeof indices]; } update = {(uintptr_t)indices, sizeof indices, {0}};
            memcpy(update.bytes, indices, sizeof indices);
            put(&list, OP_UPDATE_UB, &update, sizeof update); // forces an ordered snapshot for index-buffer binding
        }
        if (mode) RECORD(OP_DRAW_IDX,p_draw_idx,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.itype=1,.count=3,
            .base_vertex=base_vertex,.base_instance=expected[0],.instances=2,.ibuf=(uintptr_t)indices);
        else RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.first=5,.count=3,.base_instance=expected[0],.instances=2);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (size_t i=0;i<sizeof pixels;i+=4) if (memcmp(pixels+i,(uint8_t[]){0,255,0,255},4)) {
            fprintf(stderr,"draw parameters mode%d pixel%zu: %u,%u,%u,%u\n",mode,i/4,pixels[i],pixels[i+1],pixels[i+2],pixels[i+3]);abort();
        }
    }
    mtl_tex_release(&output);
    puts("Draw IDs and graphics C0 base instance, point limit and single-sample center survive draw replay");
}
static void gpu_draw_words(list_t *list, uint32_t *out, const uint32_t values[4], unsigned count) {
    push_constants(list,1,240,values,16);
    p_bind_program program={.prog=&groups,.stages=0x20};
    p_bind_buf buffer={.stage=5,.index=0,.addr=(uintptr_t)out,.size=4*count};
    p_dispatch dispatch={.x=(int32_t)count,.y=1,.z=1};
    put(list,OP_BIND_PROGRAM,&program,sizeof program); put(list,OP_BIND_SSBO,&buffer,sizeof buffer);
    put(list,OP_DISPATCH,&dispatch,sizeof dispatch);
}
static void primitive_restart(void) {
    tex_t out={.serial=0x3951,.d={.width=32,.height=16,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&out.d,&out.L);
    uint8_t indices[96]; uint32_t args[5];
    mtl_pool_add(indices,sizeof indices);mtl_pool_add(args,sizeof args);
    for (unsigned type=0; type<3; type++) for (int custom=0; custom<2; custom++)
    for (int shape=0; shape<3; shape++) for (int mode=0; mode<5; mode++) {
        if (mode>=3 && shape==2) continue;
        unsigned width=1u<<type;
        uint32_t max=type==0 ? UINT8_MAX : type==1 ? UINT16_MAX : UINT32_MAX;
        uint32_t marker=custom ? 23 : max;
        int primitive=(int[]){NVN_DRAW_PRIMITIVE_TRIANGLE_FAN,NVN_DRAW_PRIMITIVE_QUADS,NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP}[shape];
        // Two incomplete slices and adjoining restart markers before two complete polygons.
        uint32_t pattern[]={marker,9,10,marker,marker,9,10,11,12,marker,13,14,15,16,marker};
        uint32_t used=sizeof pattern/sizeof *pattern;
        if (shape==2) { pattern[7]=12; pattern[8]=11; pattern[12]=16; pattern[13]=15; }
        if (mode>=2) {
            for (int i=0;i<4;i++) pattern[i]=max-3u+(uint32_t)i;
            if (shape==2) { pattern[2]=max; pattern[3]=max-1; }
            used=4; marker=max; // Disabled: the maximum value is an ordinary right-rectangle vertex.
        }
        int indirect=mode==1;
        memset(indices,0xcc,sizeof indices);
        for (uint32_t i=0;i<used;i++) memcpy(indices+(i+1)*width,pattern+i,width);
        list_t list={0}; uint8_t pixels[32*16*4]={0};
        if (indirect) {
            gpu_draw_words(&list,args,(uint32_t[]){used,1,1,(uint32_t)-9},4);
            args[4]=7;
        }
        if (mode==4) gpu_draw_words(&list,args,(uint32_t[]){4,1,12,7},4);
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&out,.r={.width=32,.height=16,.depth=1},.mask=15,.c.f={0,0,1,1});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&out});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&restart_program,.stages=3);
        RECORD(OP_PRIM_RESTART,p_prim_restart,.on=mode<2,.index=marker);
        if (mode>=3) { // Direct and GPU-indirect nonindexed fans and quads.
            RECORD(OP_DRAW,p_draw,.prim=primitive,.first=12,.count=4,.ind=mode==4 ? (uintptr_t)args : 0);
        } else RECORD(OP_DRAW_IDX,p_draw_idx,.prim=primitive,.itype=(int)type,.count=(int)used,.base_vertex=mode<2 ? -9 : 0,
            .ibuf=(uintptr_t)indices+(indirect ? 0 : width),.ind=indirect ? (uintptr_t)args : 0);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&out,.src=(uintptr_t)pixels,.r={.width=32,.height=16,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(unsigned y=3;y<13;y++) for(unsigned x=0;x<32;x++) {
            const uint8_t *p=pixels+4*(32*y+x);
            int lit=(mode<2 && x>=2 && x<11) || (x>=21 && x<30);
            if ((p[2]==0)!=lit || (lit && p[0]+p[1]!=255) || p[3]!=255) {
                fprintf(stderr,"restart width%u custom%d shape%d mode%d pixel%u,%u: %u,%u,%u\n",
                    width,custom,shape,mode,x,y,p[0],p[1],p[2]);abort();
            }
        }
    }
    mtl_pool_del(indices);mtl_pool_del(args);mtl_tex_release(&out);
    puts("NVN restart markers split strips, fans and quads across every index width and indirect draws");
}

static uint32_t packed_vertex(const int value[4]) {
    return (uint32_t)value[0] | ((uint32_t)value[1]<<10) | ((uint32_t)value[2]<<20) | ((uint32_t)value[3]<<30);
}
static void packed_streams(void) {
    tex_t out={.serial=0x3960,.d={.width=4,.height=4,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&out.d,&out.L);
    uint32_t normalized[16]={0}, old[16]={0}, changed[18]={0};
    uint32_t *new_stream=changed+2;
    assert((uintptr_t)new_stream>UINT32_MAX);
    int expected[][4]={{81,567,1019,1},{1003,72,625,3}};
    for (int i=4;i<7;i++) {
        normalized[2*i+1]=packed_vertex((int[]){374,491,857,2});
        old[2*i+1]=packed_vertex(expected[0]);
        new_stream[2*i+1]=packed_vertex(expected[1]);
    }
    mtl_pool_add(changed,sizeof changed);
    for (int test=0;test<4;test++) {
        uint8_t pixels[64]; list_t list={0}; memset(pixels,0,sizeof pixels);
        if (test) {
            new_stream[11]=0; // Middle vertex is GPU-produced, neighbors retain their CPU values.
            gpu_draw_words(&list,new_stream+11,(uint32_t[]){packed_vertex(expected[1]),0,0,0},1);
            if (test==3) { put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);mtl_sub_commit();list.length=0; }
        }
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&out,.r={.width=4,.height=4,.depth=1},.mask=15,.c.f={0,0,1,1});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&out});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&packed_program,.stages=3);
        va_st attributes[32]={0};
        attributes[27]=(va_st){.fmt=NVN_FORMAT_RGB10A2,.offset=4,.stream=5};
        attributes[31]=(va_st){.fmt=NVN_FORMAT_RGB10A2UI,.offset=4,.stream=6};
        p_bind_vattr array={.n=32};memcpy(array.st,attributes,sizeof attributes);put(&list,OP_BIND_VATTR,&array,sizeof array);
        RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=7,.st={[5]={.stride=8},[6]={.stride=8}});
        p_bind_vbuf full={.first=5,.n=2,.r={{(uintptr_t)normalized,sizeof normalized},{(uintptr_t)old,sizeof old}}};
        put(&list,OP_BIND_VBUF,&full,offsetof(p_bind_vbuf,r)+2*sizeof full.r[0]);
        if (test==1) {
            full.first=6;full.n=1;full.r[0]=(NVNbufferRange){(uintptr_t)new_stream,16*4};
            put(&list,OP_BIND_VBUF,&full,offsetof(p_bind_vbuf,r)+sizeof full.r[0]);
        } else if (test>=2) RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=6,.addr=(uintptr_t)new_stream,.size=16*4);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=0,.index=0,.addr=(uintptr_t)expected[test!=0],.size=16);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.first=4,.count=3);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&out,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (int i=0;i<16;i++) if (memcmp(pixels+4*i,(uint8_t[]){0,255,0,255},4)) {
            fprintf(stderr,"packed streams case%d pixel%d: %u,%u,%u\n",test,i,pixels[4*i],pixels[4*i+1],pixels[4*i+2]);abort();
        }
    }
    mtl_pool_del(changed);mtl_tex_release(&out);
    puts("Packed high vertex attributes and full/compact stream rebinding use current GPU bytes");
}

static void indirect_draws(void) {
    tex_t output={.serial=0x3950,.d={.width=4,.height=4,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d,&output.L);
    static uint32_t args[1026][8];
    uint32_t count;
    uint16_t indices[]={17,17,6,7,8};
    mtl_pool_add(args,sizeof args); mtl_pool_add(&count,sizeof count); mtl_pool_add(indices,sizeof indices);
    enum { SINGLE, INLINE, RANGE };
    for (int indexed=0;indexed<2;indexed++) for (int form=SINGLE;form<=RANGE;form++) for (unsigned test=0;test<(form==RANGE ? 7u : 5u);test++) {
        list_t list={0}; uint8_t pixels[64]={0};
        int slot=form==SINGLE || test==6 ? 0 : test==5 ? 1024 : 1;
        int expected[4]={17,indexed ? -2 : 4,4,slot};
        uint32_t live=test==1 ? 0 : test==3 ? UINT32_MAX : test==5 ? 1025 : 2;
        if (test==6) expected[3]=1; // same arguments, distinct guest draw ID on the second draw
        uint32_t instances=test==2 ? 0 : 2;
        memset(args,0,sizeof args); count=1234;
        // The final record is poison: reading beyond the declared maximum would turn the target red.
        uint32_t *poison=args[test==5 ? 1025 : 2];
        poison[0]=3; poison[1]=2; poison[2]=indexed ? 2 : 4;
        poison[3]=indexed ? (uint32_t)-2 : 19; poison[4]=19;
        uint32_t gpu_args[]={3,instances,indexed ? 2 : 4,indexed ? (uint32_t)-2 : 17};
        gpu_draw_words(&list,args[slot],gpu_args,4);
        args[slot][4]=17; // indexed firstInstance comes from the adjacent CPU-initialized word
        gpu_draw_words(&list,&count,(uint32_t[]){live,0,0,0},1);
        if (test==4) { put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);mtl_sub_commit();list.length=0; }
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&output,.r={.width=4,.height=4,.depth=1},.mask=15,.c.f={0,0,1,1});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&draw_parameters_program,.stages=3);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=0,.index=0,.addr=(uintptr_t)expected,.size=sizeof expected);
        if (form==RANGE) {
            RECORD(OP_MULTI_DRAW,p_multi_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.itype=indexed?1:-1,.ibuf=(uintptr_t)indices,
                .ind=(uintptr_t)args,.cnt=(uintptr_t)&count,.max=test==3 ? 2 : INT32_MAX,.stride=test==6 ? 0 : (int64_t)sizeof args[0]);
        } else {
            unsigned n=form==SINGLE ? 1 : 2;
            for (unsigned i=0;i<n;i++) {
                uint64_t counter=form==INLINE ? (uintptr_t)&count : 0;
                if (indexed) RECORD(OP_DRAW_IDX,p_draw_idx,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.itype=1,.ibuf=(uintptr_t)indices,
                    .ind=(uintptr_t)args[i],.mdi_cnt=counter,.mdi_i=(int32_t)i);
                else RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.ind=(uintptr_t)args[i],.mdi_cnt=counter,.mdi_i=(int32_t)i);
            }
        }
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        int green=instances && (form==SINGLE || live);
        uint8_t want[]={0,green?255:0,green?0:255,255};
        for (unsigned i=0;i<sizeof pixels;i+=4) if(memcmp(pixels+i,want,4)) {
            fprintf(stderr,"indirect indexed%d form%d case%u: got %u,%u,%u,%u\n",indexed,form,test,pixels[i],pixels[i+1],pixels[i+2],pixels[i+3]);abort();
        }
    }
    mtl_pool_del(indices);mtl_pool_del(&count);mtl_pool_del(args);mtl_tex_release(&output);
    puts("GPU-written indirect and counted draws preserve vertex, instance and draw IDs");
}

static void ordered_buffers(void) {
    for (int split = 0; split <= 1; split++) {
        uint32_t words[16], copied[16], first[18], second[18];
        for (int i = 0; i < 16; i++) { words[i] = 10 + i; copied[i] = 0; }
        for (int i = 0; i < 18; i++) first[i] = second[i] = 0xcafebabe;
        list_t list = {0};
        uint32_t values[] = {101, 102, 103, 104};
        push_constants(&list, 1, 240, values, sizeof values);
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&groups, .stages=0x20);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)(words+2), .size=16);
        RECORD(OP_DISPATCH, p_dispatch, .x=4, .y=1, .z=1);
        values[0]=201; values[1]=202;
        push_constants(&list, 1, 240, values, sizeof values);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)(words+4), .size=8);
        RECORD(OP_DISPATCH, p_dispatch, .x=2, .y=1, .z=1);
        if (split) {
            put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); mtl_sub_commit(); list.length=0;
        }
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&buffer_program, .stages=0x20);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)words, .size=sizeof words);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=1, .addr=(uintptr_t)(first+1), .size=sizeof words);
        RECORD(OP_DISPATCH, p_dispatch, .x=16, .y=1, .z=1);
        uint64_t copy[] = {(uintptr_t)words, (uintptr_t)copied, sizeof words};
        put(&list, OP_COPY_B2B, copy, sizeof copy);
        RECORD(OP_CLEAR_BUF, p_clear_buf, .addr=(uintptr_t)(copied+3), .size=8, .value=777);
        struct { uint64_t address, size; uint32_t values[2]; } update = {(uintptr_t)(copied+4), 8, {888,889}};
        put(&list, OP_UPDATE_UB, &update, sizeof update);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)copied, .size=sizeof copied);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=1, .addr=(uintptr_t)(second+1), .size=sizeof copied);
        RECORD(OP_DISPATCH, p_dispatch, .x=16, .y=1, .z=1);
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        const uint32_t want[] = {10,11,101,102,201,202,16,17,18,19,20,21,22,23,24,25};
        const uint32_t patched[] = {10,11,101,777,888,889,16,17,18,19,20,21,22,23,24,25};
        for (int i = 0; i < 16; i++) {
            if (words[i]!=want[i] || copied[i]!=patched[i] || first[i+1]!=want[i]+1000 || second[i+1]!=patched[i]+1000) {
                fprintf(stderr,"ordered buffers split%d word%d: source%u copy%u first%u second%u; want %u %u %u %u\n",
                    split,i,words[i],copied[i],first[i+1],second[i+1],want[i],patched[i],want[i]+1000,patched[i]+1000); abort();
            }
        }
        assert(first[0]==0xcafebabe && first[17]==0xcafebabe && second[0]==0xcafebabe && second[17]==0xcafebabe);
    }
    puts("Overlapping GPU writes, ordered buffer copy/fill/update and same-list consumers passed");
}
static void readback_gpu_consumer(void) {
    tex_t texture = {.serial=0xb000, .d={.width=2,.height=2,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&texture.d, &texture.L);
    uint32_t pixels[6], result[6] = {0};
    for (int i=0;i<6;i++) pixels[i]=0x11111111;
    list_t list={0};
    RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&texture,.r={.width=2,.height=2,.depth=1},.mask=15,.c.f={0,1,0,1});
    RECORD(OP_COPY_STRIDE,p_copy_stride,.stride=12);
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&texture,.src=(uintptr_t)pixels,.r={.width=2,.height=2,.depth=1});
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&buffer_program,.stages=0x20);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)pixels,.size=sizeof pixels);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=1,.addr=(uintptr_t)result,.size=sizeof result);
    RECORD(OP_DISPATCH,p_dispatch,.x=6,.y=1,.z=1);
    RECORD(OP_COPY_STRIDE,p_copy_stride,.stride=0);
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for(int i=0;i<6;i++) {
        uint32_t expected=i%3==2?0x11111111:0xff00ff00;
        if(pixels[i]!=expected || result[i]!=expected+1000u) {
            fprintf(stderr,"readback GPU consumer word%d: %#x %#x expected %#x %#x\n",i,pixels[i],result[i],expected,expected+1000u);abort();
        }
    }
    mtl_tex_release(&texture);
    puts("Pitched texture readback feeds the next compute command without a host fence");
}
static void buffer_range_lifetime(void) {
    enum { COUNT=600, LARGE_BYTES=(66<<20)+256 };
    uint32_t *large=malloc(LARGE_BYTES), words[COUNT], result[COUNT];
    assert(large);memset(large,0,LARGE_BYTES);memset(words,0,sizeof words);memset(result,0,sizeof result);
    list_t list={0};
    // Exceed the usual ring size in one write. Following writes roll over while
    // it remains live, and grow ownership tracking beyond its former 512 entries.
    RECORD(OP_CLEAR_BUF,p_clear_buf,.addr=(uintptr_t)large,.size=LARGE_BYTES,.value=0x12345678);
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);
    for(int i=0;i<COUNT;i++) {
        list.length=0;
        uint32_t value[4]={(uint32_t)i+100,0,0,0};
        push_constants(&list,1,240,value,sizeof value);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)(words+i),.size=4);
        RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_exec(list.bytes);
    }
    list.length=0;
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&buffer_program,.stages=0x20);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)words,.size=sizeof words);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=1,.addr=(uintptr_t)result,.size=sizeof result);
    RECORD(OP_DISPATCH,p_dispatch,.x=COUNT,.y=1,.z=1);
    put(&list,OP_END,NULL,0);mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for(int i=0;i<COUNT;i++) assert(words[i]==(uint32_t)i+100 && result[i]==(uint32_t)i+1100);
    for(size_t i=0;i<LARGE_BYTES/4;i++) assert(large[i]==0x12345678);
    // A subsequent submission snapshots the entire open-ended range afresh;
    // it cannot reuse the retired backing from the fill above.
    mtl_pool_add(large,LARGE_BYTES);list.length=0;
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&buffer_program,.stages=0x20);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)large,.size=0);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=1,.addr=(uintptr_t)result,.size=16);
    RECORD(OP_DISPATCH,p_dispatch,.x=4,.y=1,.z=1);
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for(int i=0;i<4;i++) assert(result[i]==0x12345678+1000);
    mtl_pool_del(large);
    free(large);
    puts("600 live GPU buffer ranges and ring rollover retain their data through completion");
}
static void image_order(void) {
    tex_t data={.serial=0xb197,.d={.width=4,.height=4,.depth=2,.levels=2,.flags=NVN_TEXTURE_FLAGS_IMAGE,
        .target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_R32UI}};
    tex_t color={.serial=0xb198,.d={.width=2,.height=2,.depth=1,.levels=1,.flags=NVN_TEXTURE_FLAGS_IMAGE,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32UI}};
    nvn_tex_layout(&data.d,&data.L); nvn_tex_layout(&color.d,&color.L);
    view_t slice={.set_target=1,.target=NVN_TEXTURE_TARGET_2D,.set_levels=1,.base_level=1,.nlevels=1,
        .set_layers=1,.min_layer=1,.nlayers=1};
    mtl_reg_tex(986,&data,&slice); mtl_reg_tex(987,&color,NULL);
    const uint32_t src[8]={91,92,93,94,10,11,12,13}, dimensions[3]={2,2,1};
    uint32_t counts[5]={71,0,0,0,72}, image[8]={0}, pixel[4]={0}, mip0[4]={0};
    view_t mip={.set_levels=1,.base_level=1,.nlevels=1};
    list_t list={0};
    RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&data,.src=(uintptr_t)src,.r={.width=2,.height=2,.depth=1});
    RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&data,.has_view=1,.view=mip,.src=(uintptr_t)src,.r={.width=2,.height=2,.depth=2});
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color});
    RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)(counts+1),.size=12);
    push_constants(&list,1,240,dimensions,sizeof dimensions);
    RECORD(OP_DISPATCH,p_dispatch,.x=3,.y=1,.z=1);
    RECORD(OP_BARRIER,uint32_t,NVN_BARRIER_ORDER_INDIRECT_DATA_BIT);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&image_compute,.stages=0x20);
    RECORD(OP_BIND_IMG,p_bind_handles,.stage=5,.first=3,.n=2,.h={986,987});
    RECORD(OP_DISPATCH,p_dispatch,.ind=(uintptr_t)(counts+1));
    RECORD(OP_BARRIER,uint32_t,NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&image_draw,.stages=3);
    RECORD(OP_BIND_IMG,p_bind_handles,.stage=1,.first=3,.n=1,.h={986});
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    RECORD(OP_BARRIER,uint32_t,NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    RECORD(OP_BARRIER,uint32_t,NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);
    RECORD(OP_DISPATCH,p_dispatch,.ind=(uintptr_t)(counts+1));
    RECORD(OP_BARRIER,uint32_t,NVN_BARRIER_ORDER_PRIMITIVES_BIT);
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&data,.view=mip,.has_view=1,.src=(uintptr_t)image,.r={.width=2,.height=2,.depth=2});
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)pixel,.r={.width=2,.height=2,.depth=1});
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&data,.src=(uintptr_t)mip0,.r={.width=2,.height=2,.depth=1});
    put(&list,OP_END,NULL,0);
    mtl_queue_activate(0xb197);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    assert(counts[0]==71 && counts[4]==72 && !memcmp(counts+1,dimensions,sizeof dimensions));
    assert(!memcmp(mip0,src,sizeof mip0) && !memcmp(image,src,sizeof mip0));
    for (uint32_t i=0;i<4;++i) {
        if (pixel[i]!=15+2*i || image[4+i]!=35+5*i) die("ordered image pixel %u: attachment %u, storage %u expected %u/%u",
            i,pixel[i],image[4+i],15+2*i,35+5*i);
    }
    mtl_reg_tex(986,NULL,NULL); mtl_reg_tex(987,NULL,NULL);
    mtl_tex_release(&data); mtl_tex_release(&color); mtl_queue_forget(0xb197);
    puts("Vulkan direct and GPU-indirect dispatch, fragment image atomics, attachment reads and transfer barriers passed");
}

// Each output reads both attachments as they stood before this draw. The
// attachments deliberately share their format and dimensions.
enum { FEEDBACK_WIDTHS = 19, FEEDBACK_RUNS = FEEDBACK_WIDTHS * 2 };
typedef struct { uint8_t slot[2][FEEDBACK_WIDTHS * 2 * 4]; } feedback_pixels;

static uint64_t feedback_submit(unsigned run, unsigned width, feedback_pixels *pixels) {
    tex_t target[2];
    uint8_t initial[][4] = {{16,32,48,255},{64,80,96,255}};
    list_t list = {0};
    for (int i=0; i<2; ++i) {
        target[i] = (tex_t){.serial=0x8fb000+2*run+i, .d={.width=width,.height=2,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&target[i].d,&target[i].L);
        mtl_reg_tex(981+i, &target[i], NULL);
        p_clear_tex clear = {.tex=&target[i], .r={.width=width,.height=2,.depth=1},.mask=15};
        for (int c=0; c<4; ++c) clear.c.f[c] = initial[i][c] / 255.0f;
        put(&list, OP_CLEAR_TEX, &clear, sizeof clear);
        RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1,.slot=i,.h=981+i);
    }
    RECORD(OP_SET_RT,p_set_rt,.n=3,.color={[0]=&target[0],[2]=&target[1]});
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&feedback_program,.stages=3);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    RECORD(OP_BARRIER,uint32_t,NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    for (int i=0; i<2; ++i)
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target[i],.src=(uintptr_t)pixels->slot[i],.r={.width=width,.height=2,.depth=1});
    put(&list, OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(list.bytes);
    uint64_t fence = mtl_sub_commit();
    for (int i=0; i<2; ++i) {
        mtl_reg_tex(981+i, NULL, NULL);
        mtl_tex_release(&target[i]);
    }
    return fence;
}

static void attachment_feedback(void) {
    // Change dimensions past the snapshot cache capacity, reuse the guest
    // descriptor IDs immediately, and only wait after all submissions.
    feedback_pixels results[FEEDBACK_RUNS] = {0};
    const uint8_t want[][4] = {{32,64,16,255},{16,32,64,255}};
    void mtl_fb_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
    uint64_t before, after, reuse, tile, wait;
    mtl_fb_stats(&before, &reuse, &tile, &wait);
    uint64_t fence = 0;
    mtl_queue_activate(0xb190);
    for (unsigned run=0; run<FEEDBACK_RUNS; ++run)
        fence = feedback_submit(run, 1 + run % FEEDBACK_WIDTHS, &results[run]);
    assert(mtl_wait(fence, UINT64_MAX));
    mtl_fb_stats(&after, &reuse, &tile, &wait);
    if (after-before != 4*FEEDBACK_RUNS)
        die("expected %u feedback snapshots, got %llu", 4*FEEDBACK_RUNS, (unsigned long long)(after-before));
    for (unsigned run=0; run<FEEDBACK_RUNS; ++run) {
        unsigned width = 1 + run % FEEDBACK_WIDTHS;
        for (int i=0; i<2; ++i) for (unsigned p=0; p<2*width; ++p) {
            const uint8_t *got = results[run].slot[i]+4*p;
            if (memcmp(got,want[i],4)) die("feedback run %u width %u slot %d pixel %u: %u/%u/%u expected %u/%u/%u",
                run,width,2*i,p,got[0],got[1],got[2],want[i][0],want[i][1],want[i][2]);
        }
    }
    mtl_queue_forget(0xb190);
    puts("Vulkan sparse MRT feedback preserved both attachments across the guest barrier");
}

static void layered_feedback(void) {
    tex_t array = {.serial=0xb193,.d={.width=4,.height=4,.depth=3,.levels=2,
        .target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_RGBA8}};
    tex_t depth = {.serial=0xb194,.d={.width=2,.height=2,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_DEPTH32F}};
    nvn_tex_layout(&array.d,&array.L);
    nvn_tex_layout(&depth.d,&depth.L);
    view_t slice = {.set_target=1,.target=NVN_TEXTURE_TARGET_2D,.set_levels=1,.base_level=1,.nlevels=1,
        .set_layers=1,.min_layer=1,.nlayers=1};
    view_t first=slice, second=slice, other=slice, base=slice;
    first.set_swizzle=1;
    memcpy(first.swz,(uint8_t[]){3,2,4,5},4);   // sample green, red, blue, alpha
    second.min_layer=2; second.set_format=1; second.format=NVN_FORMAT_BGRA8;
    other.min_layer=0;
    base.base_level=0;
    mtl_reg_tex(983,&array,&first);
    mtl_reg_tex(984,&array,&second);
    mtl_reg_tex(985,&array,&other);
    uint8_t guard[16], retained[2][16], pixels[2][2][16];
    for (int i=0;i<4;++i) memcpy(guard+4*i,(uint8_t[]){7,17,27,255},4);
    list_t list={0};
    RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&array,.has_view=1,.view=other,.src=(uintptr_t)guard,.r={.width=2,.height=2,.depth=1});
    RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&array,.has_view=1,.view=base,.src=(uintptr_t)guard,.r={.width=2,.height=2,.depth=1});
    p_set_rt targets={.n=3,.color={[0]=&array,[2]=&array},.has_cview={[0]=1,[2]=1},.cview={[0]=slice,[2]=second}};
    put(&list, OP_SET_RT, &targets, sizeof targets);
    RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=15,.rgba={16/255.f,32/255.f,48/255.f,1});
    RECORD(OP_CLEAR_COLOR,p_clear_color,.index=2,.mask=15,.rgba={64/255.f,80/255.f,96/255.f,1});
    RECORD(OP_SET_RT,p_set_rt,.depth=&depth);
    RECORD(OP_CLEAR_DS,p_clear_ds,.depth=.25f,.depth_mask=1);
    put(&list, OP_SET_RT, &targets, sizeof targets);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&feedback_program,.stages=3);
    RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1,.slot=1,.h=984);
    for (int draw=0;draw<2;++draw) {
        // First read the swapped view of attachment zero, then sample a
        // different layer of that same image which neither attachment writes.
        RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1,.slot=0,.h=draw?985:983);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&array,.has_view=1,.view=slice,.src=(uintptr_t)pixels[draw][0],.r={.width=2,.height=2,.depth=1});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&array,.has_view=1,.view=second,.src=(uintptr_t)pixels[draw][1],.r={.width=2,.height=2,.depth=1});
    }
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&array,.has_view=1,.view=other,.src=(uintptr_t)retained[0],.r={.width=2,.height=2,.depth=1});
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&array,.has_view=1,.view=base,.src=(uintptr_t)retained[1],.r={.width=2,.height=2,.depth=1});
    float z[4]={0};
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&depth,.src=(uintptr_t)z,.r={.width=2,.height=2,.depth=1});
    put(&list,OP_END,NULL,0);
    mtl_queue_activate(0xb193);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    const uint8_t want[2][2][4]={{{64,32,16,255},{80,64,16,255}},{{16,7,17,255},{64,16,17,255}}};
    for (int draw=0;draw<2;++draw) for (int i=0;i<2;++i) for (int p=0;p<4;++p) {
        uint8_t *got=pixels[draw][i]+4*p;
        if (memcmp(got,want[draw][i],4)) die("layer feedback draw %d slot %d pixel %d: %u/%u/%u expected %u/%u/%u",
            draw,2*i,p,got[0],got[1],got[2],want[draw][i][0],want[draw][i][1],want[draw][i][2]);
    }
    assert(!memcmp(guard,retained[0],16) && !memcmp(guard,retained[1],16));
    for (int i=0;i<4;++i) assert(z[i]==.25f);
    for (int i=983;i<=985;++i) mtl_reg_tex(i,NULL,NULL);
    mtl_tex_release(&array); mtl_tex_release(&depth); mtl_queue_forget(0xb193);
    puts("Vulkan feedback views preserved separate layers, mip guards, BGRA format and component swizzle");
}

static void graphics_storage(void) {
    tex_t target={.serial=0xb100,.d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    tex_t data={.serial=0xb19a,.d={.width=5,.height=2,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,
        .format=NVN_FORMAT_R32UI,.flags=NVN_TEXTURE_FLAGS_IMAGE}};
    nvn_tex_layout(&target.d,&target.L); nvn_tex_layout(&data.d,&data.L);
    mtl_reg_tex(988,&data,NULL);
    for(int split=0;split<2;split++) {
        mtl_queue_activate(0xb100+split);
        mtl_set_origin_lower_left(split); mtl_set_queue_origin_lower_left(split);
        uint32_t vertices[5]={0xcafebabe,10,10,10,0xcafebabe}, fragments[66], vertex_result[3]={0}, fragment_result[64]={0};
        for(int i=0;i<66;i++) fragments[i]=i==0||i==65?0xcafebabe:20;
        uint8_t pixels[8*8*4]={0}, discarded[8*8*4]={0};list_t list={0};
        uint32_t seed[10], image_steps[3][10];
        for (int i=0;i<10;i++) seed[i]=0xf1000000u+3*i;
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&data,.src=(uintptr_t)seed,.r={.width=5,.height=2,.depth=1});
        RECORD(OP_BIND_IMG,p_bind_handles,.stage=0,.first=2,.n=1,.h={988});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0,0,1,1});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&discard_image_program,.stages=3);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=0,.index=0,.addr=(uintptr_t)(vertices+1),.size=12);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)(fragments+1),.size=256);
        RECORD(OP_RAST_DISCARD,p_rast_discard,.on=1);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)discarded,.r={.width=8,.height=8,.depth=1});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&data,.src=(uintptr_t)image_steps[0],.r={.width=5,.height=2,.depth=1});
        RECORD(OP_SET_RT,p_set_rt,.n=0);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&data,.src=(uintptr_t)image_steps[1],.r={.width=5,.height=2,.depth=1});
        RECORD(OP_RAST_DISCARD,p_rast_discard,.on=0);
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
        // Both clipped-away draws must still execute every vertex invocation.
        const p_scissor clipped[] = {{2,2,0,3}, {12,1,4,3}};
        for (unsigned i=0; i<sizeof clipped/sizeof *clipped; ++i) {
            put(&list, OP_SCISSOR, &clipped[i], sizeof clipped[i]);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            put(&list,OP_BARRIER,NULL,0);
        }
        RECORD(OP_SCISSOR,p_scissor,.x=0,.y=0,.w=8,.h=8);
        for(int draw=0;draw<3;draw++) {
            uint32_t value=20+draw;push_constants(&list,0,240,&value,sizeof value);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            put(&list,OP_BARRIER,NULL,0);
        }
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&data,.src=(uintptr_t)image_steps[2],.r={.width=5,.height=2,.depth=1});
        if(split) { put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);mtl_sub_commit();list.length=0; }
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&buffer_program,.stages=0x20);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)(vertices+1),.size=12);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=1,.addr=(uintptr_t)vertex_result,.size=sizeof vertex_result);
        RECORD(OP_DISPATCH,p_dispatch,.x=3,.y=1,.z=1);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)(fragments+1),.size=256);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=1,.addr=(uintptr_t)fragment_result,.size=sizeof fragment_result);
        RECORD(OP_DISPATCH,p_dispatch,.x=64,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(int i=0;i<3;i++) if(vertices[i+1]!=17||vertex_result[i]!=1017) {
            fprintf(stderr,"graphics storage split%d vertex%d: %u/%u expected 17/1017\n",split,i,vertices[i+1],vertex_result[i]);abort();
        }
        for(int i=0;i<64;i++) assert(!memcmp(discarded+4*i,(uint8_t[]){0,0,255,255},4));
        for(int i=0;i<64;i++) assert(fragments[i+1]==23 && fragment_result[i]==1023 && !memcmp(pixels+4*i,(uint8_t[]){0,255,0,255},4));
        assert(vertices[0]==0xcafebabe&&vertices[4]==0xcafebabe&&fragments[0]==0xcafebabe&&fragments[65]==0xcafebabe);
        const uint32_t invocations[]={1,2,7};
        for (int step=0;step<3;step++) for (int i=0;i<10;i++) {
            uint32_t want=seed[i]+(i>=1 && i<=3 ? invocations[step] : 0);
            if (image_steps[step][i]!=want) die("discard storage image: step %d pixel %d got %#x expected %#x",
                                                step,i,image_steps[step][i],want);
        }
        mtl_queue_forget(0xb100+split);
    }
    mtl_set_origin_lower_left(0);
    mtl_reg_tex(988,NULL,NULL); mtl_tex_release(&data); mtl_tex_release(&target);
    puts("Vertex image atomics and storage writes survive raster discard, attachmentless draws, compute consumers and fences");
}
static void simultaneous_aliases(void) {
    for(int mode=0;mode<9;mode++) {
        uint32_t storage[260], result[6]={0xcafebabe,0,0,0,0,0xcafebabe};
        for(int i=0;i<260;i++) storage[i]=0xcafebabe;
        const uint32_t deltas[]={0,64,1,3,4,7,63,65,4};
        uint32_t delta=deltas[mode];
        if(mode==8) mtl_pool_add(storage+1,1024);
        list_t list={0};push_constants(&list,1,240,&delta,sizeof delta);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&alias_program,.stages=0x20);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)(storage+1),.size=mode==8?0:mode?512:256);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=1,.addr=(uintptr_t)(storage+1+delta),.size=mode==8?0:mode?256:512);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=2,.addr=(uintptr_t)(result+1),.size=16);
        RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        const uint32_t expected[]={1001,2002,3003};
        for(int i=0;i<3;i++) if(result[i+1]!=expected[i]) {
            fprintf(stderr,"simultaneous aliases mode%d word%d: %u expected %u\n",mode,i,result[i+1],expected[i]);abort();
        }
        for(int i=0;i<260;i++) assert(storage[i]==(i==1+delta?1001:i==2+delta?2002:i==3+delta?3004:0xcafebabe));
        assert(result[4]==(mode==8?508:192)&&result[0]==0xcafebabe&&result[5]==0xcafebabe);
        if(mode==8) mtl_pool_del(storage+1);
    }
    puts("Overlapping storage bindings share writes and atomics within one dispatch");
}
static void graphics_storage_alias(void) {
    tex_t target={.serial=0xb101,.d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&target.d,&target.L);
    uint32_t storage[68],result[65]={0};for(int i=0;i<68;i++) storage[i]=10;
    list_t list={0};uint32_t expected=10;push_constants(&list,0,240,&expected,sizeof expected);
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&storage_program,.stages=3);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=0,.index=0,.addr=(uintptr_t)(storage+1),.size=12);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)(storage+2),.size=256);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&buffer_program,.stages=0x20);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)(storage+1),.size=260);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=1,.addr=(uintptr_t)result,.size=sizeof result);
    RECORD(OP_DISPATCH,p_dispatch,.x=65,.y=1,.z=1);
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for(int i=1;i<=65;i++) { uint32_t want=i==2||i==3?12:11;assert(storage[i]==want&&result[i-1]==want+1000); }
    assert(storage[0]==10&&storage[66]==10&&storage[67]==10);
    mtl_tex_release(&target);
    puts("Vertex and fragment atomics share partially overlapping storage bindings");
}
static void gpu_indices(void) {
    tex_t target={.serial=0xb200,.d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&target.d,&target.L);
    for(int split=0;split<2;split++) for(int type=0;type<3;type++) {
        uint32_t indices[3]={~0u,~0u,~0u}, values[4]={0,1,2,0}, expected[4]={0};
        if(type==0) values[0]=0x00020100;
        if(type==1) { values[0]=0x00010000;values[1]=2; }
        uint8_t pixels[8*8*4]={0};list_t list={0};
        push_constants(&list,1,240,values,sizeof values);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)indices,.size=4*(type+1));
        RECORD(OP_DISPATCH,p_dispatch,.x=type+1,.y=1,.z=1);
        if(split) { put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);mtl_sub_commit();list.length=0; }
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&target,.r={.width=8,.height=8,.depth=1},.mask=15,.c.f={0,0,1,1});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&draw_parameters_program,.stages=3);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=0,.index=0,.addr=(uintptr_t)expected,.size=sizeof expected);
        RECORD(OP_DRAW_IDX,p_draw_idx,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.itype=type,.count=3,.instances=1,.ibuf=(uintptr_t)indices);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(int i=0;i<64;i++) if(memcmp(pixels+4*i,(uint8_t[]){0,255,0,255},4)) {
            fprintf(stderr,"GPU index split%d type%d pixel%d: %u,%u,%u,%u\n",split,type,i,pixels[4*i],pixels[4*i+1],pixels[4*i+2],pixels[4*i+3]);abort();
        }
    }
    mtl_tex_release(&target);
    puts("GPU-generated 8/16/32-bit indices reach indexed draws within and across submissions");
}
static void pool_ranges(void) {
    enum { COUNT=4100, BYTES=32 };
    uint8_t *mem=calloc(COUNT,BYTES);assert(mem);
    for(int i=0;i<COUNT;i++) mtl_pool_add(mem+i*BYTES,BYTES);
    for(int i=0;i<COUNT;i++) {
        void *base=NULL;uint64_t off=0;
        assert(mtl_addr((uintptr_t)(mem+i*BYTES+3),BYTES-3,&base,&off));
        assert(base==mem+i*BYTES&&off==3);
    }
    assert(!mtl_addr((uintptr_t)mem+1,UINT64_MAX,NULL,NULL));
    assert(!mtl_addr(UINT64_MAX-8,32,NULL,NULL));
    for(int i=0;i<COUNT;i++) mtl_pool_del(mem+i*BYTES);
    assert(!mtl_addr((uintptr_t)mem,1,NULL,NULL));
    mtl_pool_add(mem,BYTES/2);
    assert(mtl_addr((uintptr_t)mem+BYTES/2-1,1,NULL,NULL));
    assert(!mtl_addr((uintptr_t)mem+BYTES/2,1,NULL,NULL));
    mtl_pool_del(mem);free(mem);
    puts("Pool lookup retains 4100 registrations, rejects overflow and respects reused extents");
}
static void retired_texture_pool(void) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    pool_t *pool=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(pool!=MAP_FAILED);
    uint8_t storage[64*8];memset(storage,153,sizeof storage);
    *pool=(pool_t){.flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED,.mem=storage,.size=sizeof storage};
    tex_t source={.serial=0xc000,.mem=storage,.pool=pool,.d={.width=64,.height=8,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R8,.flags=NVN_TEXTURE_FLAGS_LINEAR,.stride=64}};
    tex_t cold=source;cold.serial++;
    tex_t output={.serial=0xc003,.d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&source.d,&source.L);cold.L=source.L;nvn_tex_layout(&output.d,&output.L);
    mtl_reg_tex(920,&source,NULL);mtl_reg_tex(921,&cold,NULL);
    tex_t replacement=source;replacement.serial+=2;
    for(int phase=0;phase<4;phase++) {
        if(phase==2) {
            assert(!mprotect(pool,page,PROT_READ|PROT_WRITE));
            *pool=(pool_t){.flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED,.mem=storage,.size=sizeof storage};
            memset(storage,221,sizeof storage);
        }
        if(phase==3) mtl_reg_tex(920,&replacement,NULL);
        for(int slot=0;slot<(phase?2:1);slot++) {
            uint8_t pixels[256]={0};list_t list={0};
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_copy_program,.stages=3);
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=1|BIND_TEX1_NOBASE,.slot=0,.h=920+slot);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);
            uint64_t fence=mtl_sub_commit();
            if(!phase) { mtl_alias_drop_pool(pool);assert(!mprotect(pool,page,PROT_NONE)); }
            assert(mtl_wait(fence,UINT64_MAX));
            uint8_t want[]={phase==0?153:phase==3&&slot==0?221:0,0,0,255};
            for(int i=0;i<64;i++) if(memcmp(pixels+4*i,want,4)) {
                fprintf(stderr,"retired pool phase%d slot%d pixel%d: %u expected%u\n",phase,slot,i,pixels[4*i],want[0]);abort();
            }
        }
    }
    mtl_alias_drop_pool(pool);mtl_tex_release(&source);mtl_tex_release(&cold);mtl_tex_release(&replacement);mtl_tex_release(&output);
    assert(!munmap(pool,page));
    puts("Pool retirement preserves submitted draws, invalidates warm/cold IDs and requires registration after address reuse");
}
static void separate_descriptors(int mixed_formats) {
    int caps; replay_features(&caps,NULL);
    tex_t images[2],output={.serial=0xd002,.d={.width=4,.height=4,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d,&output.L);
    for(int i=0;i<2;i++) {
        images[i]=(tex_t){.serial=0xd000+i,.d={.width=1,.height=1,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_3D,.format=mixed_formats && i ? NVN_FORMAT_RGBA16F : NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&images[i].d,&images[i].L);mtl_reg_tex(2053+i,&images[i],NULL);
    }
    samp_par edge={0},border={.ws=NVN_WRAP_MODE_CLAMP_TO_BORDER,
        .border={96.f/255,32.f/255,159.f/255,127.f/255}};
    mtl_reg_samp(513,&edge);mtl_reg_samp(514,&border);
    uint64_t handles[8]={0},samplers[8]={0};mtl_pool_add(handles,sizeof handles);mtl_pool_add(samplers,sizeof samplers);
    float positions[]={-1,-1,3,-1,-1,3},white[]={1,1,1,1};
    for(int mode=0;mode<4;mode++) for(int pass=0;pass<10;pass++) {
        uint8_t pixels[64]={0};list_t list={0};
        struct { uint32_t image,sampler;float coordinate;uint32_t pad; } params={pass&1,(pass>>1)&1,pass&4?1.5f:0.5f,0};
        // Keep the other array element valid while this one is missing.
        mtl_reg_tex(2053,pass>=8?NULL:&images[0],NULL);
        for(int i=0;i<2;i++) {
            p_clear_tex clear={.tex=&images[i],.r={.width=1,.height=1,.depth=1},.mask=15};clear.c.f[i+1]=1;clear.c.f[3]=1;
            put(&list,OP_CLEAR_TEX,&clear,sizeof clear);
        }
        if(mode==1) {
            memset(handles,0,sizeof handles);memset(samplers,0,sizeof samplers);
            uint32_t values[4]={5,0,6,0};push_constants(&list,1,240,values,sizeof values);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)&handles[2],.size=16);
            RECORD(OP_DISPATCH,p_dispatch,.x=4,.y=1,.z=1);
            values[0]=1u<<20;values[2]=2u<<20;push_constants(&list,1,240,values,sizeof values);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)&samplers[3],.size=16);
            RECORD(OP_DISPATCH,p_dispatch,.x=4,.y=1,.z=1);
        }
        RECORD(OP_SET_TEX_POOL,p_set_pool,.pool=&texture_pool);
        RECORD(OP_SET_SAMP_POOL,p_set_pool,.pool=&alternate_sampler_pool);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=7,.addr=(uintptr_t)handles,.size=sizeof handles);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=8,.addr=(uintptr_t)samplers,.size=sizeof samplers);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=9,.addr=(uintptr_t)&params,.size=sizeof params);
        RECORD(OP_BIND_TEX,p_bind_handles,.stage=1,.first=3,.n=2,.h={2053,2054});
        RECORD(OP_BIND_TEX,p_bind_handles,.stage=1,.first=10,.n=2,.h={513ull<<20,514ull<<20});
        RECORD(OP_BIND_SAMP,p_bind_handles,.stage=1,.first=5,.n=2,.h={513ull<<20,514ull<<20});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&separate_program[mode],.stages=3);
        push_constants(&list,0,240,white,sizeof white);
        va_st va={.fmt=NVN_FORMAT_RG32F};p_bind_vattr attr={.n=1};memcpy(attr.st,&va,sizeof va);put(&list,OP_BIND_VATTR,&attr,sizeof attr);
        RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=1,.st={{.stride=8}});
        RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=sizeof positions,.addr=(uintptr_t)positions);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        uint8_t want[4]={0,params.image?0:255,params.image?255:0,255};if((pass&6)==6) {
            int exact=caps & (mixed_formats ? 2 : 1);
            if (exact) memcpy(want,(uint8_t[]){96,32,159,127},4);
            else memset(want,0,4);
        }
        if(pass==8) want[1]=0;
        for(int i=0;i<16;i++) if(memcmp(pixels+4*i,want,4)) {
            fprintf(stderr,"separate descriptors mode%d pass%d pixel%d: %u,%u,%u,%u\n",mode,pass,i,pixels[4*i],pixels[4*i+1],pixels[4*i+2],pixels[4*i+3]);abort();
        }
    }
    mtl_pool_del(handles);mtl_pool_del(samplers);
    for(int i=0;i<2;i++) { mtl_reg_tex(2053+i,NULL,NULL);mtl_tex_release(&images[i]); }
    mtl_tex_release(&output);
    puts("Separate texture/sampler arrays use independent slots, GPU uniform handles, texture-packed and fixed sampler IDs");
}
static void descriptor_lifetime(void) {
    enum { N = 100 };
    tex_t source[2], output = {.serial = 0xe002, .d = {.width = 8, .height = 8, .depth = 1, .levels = 1,
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    samp_par nearest = {0}; mtl_reg_samp(950, &nearest);
    for (int i = 0; i < 2; i++) {
        source[i] = output; source[i].serial = 0xe000 + i; mtl_reg_tex(950 + i, &source[i], NULL);
    }
    mtl_queue_activate(950);
    // More than three submissions reuse every context. Keep all sets alive
    // until its fence: earlier draws/dispatches must retain their descriptors.
    for (int round = 0; round < 4; round++) {
        uint32_t words[N][4] = {0}; uint8_t pixels[N][256] = {0};
        uint64_t before = mtl_last_fence(); mtl_sub_begin();
        for (int i = 0; i < N; i++) {
            list_t list = {0}; uint32_t values[4] = {1000u * round + 4u * i + 1, 2, 3, 4};
            push_constants(&list, 1, 240, values, sizeof values);
            RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &groups, .stages = 0x20);
            RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)words[i], .size = sizeof words[i]);
            RECORD(OP_DISPATCH, p_dispatch, .x = 4, .y = 1, .z = 1);
            put(&list, OP_END, NULL, 0); mtl_exec(list.bytes);
        }
        for (int i = 0; i < N; i++) {
            list_t list = {0};
            if (!i) for (int j = 0; j < 2; j++) {
                RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &source[j], .r = {.width = 8, .height = 8, .depth = 1}, .mask = 15, .c.f = {j ? 1 : 0, 0, 0, 1});
            }
            RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
            RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &depth_copy_program, .stages = 3);
            RECORD(OP_BIND_TEX1, p_bind_tex1, .stage = 1 | BIND_TEX1_NOBASE, .slot = 0, .h = (950 + (i & 1)) | (950ull << 20));
            RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
            RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)pixels[i], .r = {.width = 8, .height = 8, .depth = 1});
            put(&list, OP_END, NULL, 0); mtl_exec(list.bytes);
        }
        uint64_t fence = mtl_sub_commit(); assert(fence == before + 1); assert(mtl_wait(fence, UINT64_MAX));
        for (int i = 0; i < N; i++) {
            uint32_t want[4] = {1000u * round + 4u * i + 1, 2, 3, 4};
            if (memcmp(words[i], want, sizeof want)) fprintf(stderr, "descriptor lifetime round%d dispatch%d: %u expected%u\n", round, i, words[i][0], want[0]);
            assert(!memcmp(words[i], want, sizeof want));
            uint8_t color[4] = {i & 1 ? 255 : 0, 0, 0, 255};
            for (int p = 0; p < 64; p++) assert(!memcmp(pixels[i] + p * 4, color, 4));
        }
    }
    for (int i = 0; i < 2; i++) { mtl_reg_tex(950 + i, NULL, NULL); mtl_tex_release(&source[i]); }
    mtl_tex_release(&output); mtl_queue_forget(950);
    puts("Descriptor pools preserve 400 dispatches and 400 draws across exhaustion and context reuse without submission cuts");
}
static void typed_buffers(void) {
    enum { N = 4 };
    const int offsets[] = {0,4,12,60,256};
    for (int missing = 0; missing < 2; missing++) for (int split = 0; split < 2; split++) for (unsigned c = 0; c < sizeof offsets/sizeof *offsets; c++) {
        int offset = offsets[c];
        // Both array elements cover the same bytes shifted by one R32F texel.
        const float expected_samples[N+1]={.25f,.5f,.75f,1,2};
        float samples[N+1]={0,0,0,0,2}; float *f[]={samples,samples+1};
        int32_t signed_values[N] = {-10,-20,-30,-40}; uint32_t unsigned_values[N] = {10,20,30,40};
        uint32_t storage[256], previous[N] = {0}, result[N][8] = {0}, consumer[N] = {0};
        for (int i = 0; i < 256; i++) storage[i] = 0xabcdef01;
        uint32_t *target = storage + 64 + offset / 4;
        for (int i = 0; i < N; i++) target[i] = 0; // produced on the GPU before the padded alias snapshot
        void *mem[] = {f[0],f[1],signed_values,unsigned_values,target,previous};
        NVNformat formats[] = {NVN_FORMAT_R32F,NVN_FORMAT_R32F,NVN_FORMAT_R32I,NVN_FORMAT_R32UI,NVN_FORMAT_R32UI,NVN_FORMAT_R32UI};
        tex_t tex[6]; pool_t pools[6];
        for (int i = 0; i < 6; i++) {
            pools[i] = (pool_t){.mem = mem[i], .size = 4*N, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED};
            tex[i] = (tex_t){.serial = 0xf000 + missing*1024 + split*16 + offset + i, .mem = mem[i], .pool = &pools[i],
                .d = {.width = N, .height = 1, .depth = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_BUFFER, .format = formats[i]}};
            nvn_tex_layout(&tex[i].d, &tex[i].L); mtl_reg_tex(960 + i, &tex[i], NULL);
        }
        // A format view changes interpretation while retaining the same bytes.
        tex[3].d.format = NVN_FORMAT_R32F;
        view_t view = {.set_format = 1, .format = NVN_FORMAT_R32UI}; mtl_reg_tex(963, &tex[3], &view);
        if (missing) mtl_reg_tex(961, NULL, NULL);
        mtl_queue_activate(960); mtl_sub_begin(); list_t list = {0};
        push_constants(&list,1,240,expected_samples,N*sizeof(float));
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)samples,.size=N*sizeof(float));
        RECORD(OP_DISPATCH,p_dispatch,.x=N,.y=1,.z=1);
        uint32_t values[N] = {70,80,90,100}; push_constants(&list, 1, 240, values, sizeof values);
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &groups, .stages = 0x20);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)previous, .size = sizeof previous);
        RECORD(OP_DISPATCH, p_dispatch, .x = N, .y = 1, .z = 1);
        uint32_t initial[N] = {500,501,502,503}; push_constants(&list, 1, 240, initial, sizeof initial);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)target, .size = 4*N);
        RECORD(OP_DISPATCH, p_dispatch, .x = N, .y = 1, .z = 1);
        put(&list, OP_END, NULL, 0); mtl_exec(list.bytes); list.length = 0;
        if (split) { mtl_sub_commit(); mtl_sub_begin(); }
        uint32_t params[N] = {offset / 4, 0, 0, 0}; push_constants(&list, 1, 240, params, sizeof params);
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &texel_program, .stages = 0x20);
        RECORD(OP_BIND_TEX, p_bind_handles, .stage = 5, .first = 0, .n = 4, .h = {960,961,962,963});
        RECORD(OP_BIND_IMG, p_bind_handles, .stage = 5, .first = 0, .n = 2, .h = {964,965});
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)(target-offset/4), .size = offset+4*N);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 1, .addr = (uintptr_t)result, .size = sizeof result);
        RECORD(OP_DISPATCH, p_dispatch, .x = N, .y = 1, .z = 1);
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &buffer_program, .stages = 0x20);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)target, .size = 4*N);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 1, .addr = (uintptr_t)consumer, .size = sizeof consumer);
        RECORD(OP_DISPATCH, p_dispatch, .x = N, .y = 1, .z = 1);
        put(&list, OP_END, NULL, 0); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int i = 0; i < N; i++) {
            uint32_t want[8] = {500+i,510+i,610+i,(uint32_t)(expected_samples[i]*16),missing?0:(uint32_t)(expected_samples[i+1]*16),(uint32_t)(signed_values[i]+100),unsigned_values[i]+values[i],3*N};
            if (memcmp(result[i], want, sizeof want)) fprintf(stderr, "typed buffers split%d offset%d element%d: %u,%u,%u expected%u,%u,%u\n", split,offset,i,result[i][0],result[i][1],result[i][2],want[0],want[1],want[2]);
            assert(!memcmp(result[i], want, sizeof want)); assert(target[i] == 610u+i); assert(consumer[i] == 1610u+i);
        }
        for (int i = 0; i < 256; i++) if (i < 64+offset/4 || i >= 64+offset/4+N) assert(storage[i] == 0xabcdef01);
        for (int i = 0; i < 6; i++) { mtl_reg_tex(960+i, NULL, NULL); mtl_tex_release(&tex[i]); }
        mtl_queue_forget(960);
    }
    puts("Typed float/int/uint buffer views, arrays, sizes, image/SSBO atomics, prior GPU writes and following consumers passed");
}
static void typed_graphics(void) {
    tex_t output = {.serial = 0xf800, .d = {.width = 8, .height = 8, .depth = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    for (int split = 0; split < 2; split++) {
        uint32_t counters[66], consumer[64] = {0}, source = 0;
        for (int i = 0; i < 66; i++) counters[i] = 100;
        counters[0] = counters[65] = 0xabcdef01;
        tex_t image = {.serial = 0xf810 + split*2, .mem = (uint8_t *)(counters+1), .d = {.width = 64, .height = 1, .depth = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_R32UI}};
        tex_t sample = image; sample.serial++; sample.mem = (uint8_t *)&source; sample.d.width = 1;
        nvn_tex_layout(&image.d, &image.L); nvn_tex_layout(&sample.d, &sample.L);
        mtl_reg_tex(970, &image, NULL); mtl_reg_tex(971, &sample, NULL);
        uint64_t handles[2] = {0,0}; uint8_t pixels[3][256] = {0};
        mtl_pool_add(handles, sizeof handles); mtl_queue_activate(970); mtl_sub_begin();
        for (int draw = 0; draw < 3; draw++) {
            list_t list = {0};
            // Produce the TEXU handle on the GPU; the draw's CPU preflight
            // must resolve it before allocating its typed view and storage.
            if (!draw) {
                uint32_t values[4] = {971,0,0,0}; push_constants(&list, 1, 240, values, sizeof values);
                RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &groups, .stages = 0x20);
                RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)&handles[1], .size = 8);
                RECORD(OP_DISPATCH, p_dispatch, .x = 2, .y = 1, .z = 1);
            }
            struct { uint64_t addr, size; uint32_t value, pad; } update = {(uintptr_t)&source,4,21u*(draw+1),0};
            put(&list, OP_UPDATE_UB, &update, sizeof update);
            RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
            RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &texel_graphics_program, .stages = 3);
            RECORD(OP_BIND_IMG, p_bind_handles, .stage = 1, .first = 0, .n = 1, .h = {970});
            RECORD(OP_BIND_IMG, p_bind_handles, .stage = 0, .first = 0, .n = 1, .h = {970});
            RECORD(OP_BIND_UBO, p_bind_buf, .stage = 1, .index = 7, .addr = (uintptr_t)handles, .size = sizeof handles);
            RECORD(OP_BIND_UBO, p_bind_buf, .stage = 0, .index = 7, .addr = (uintptr_t)handles, .size = sizeof handles);
            RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
            RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)pixels[draw], .r = {.width = 8, .height = 8, .depth = 1});
            put(&list, OP_END, NULL, 0); mtl_exec(list.bytes);
            if (split) { mtl_sub_commit(); mtl_sub_begin(); }
        }
        list_t list = {0};
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &buffer_program, .stages = 0x20);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)(counters+1), .size = 256);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 1, .addr = (uintptr_t)consumer, .size = sizeof consumer);
        RECORD(OP_DISPATCH, p_dispatch, .x = 64, .y = 1, .z = 1);
        put(&list, OP_END, NULL, 0); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int i = 0; i < 64; i++) { assert(counters[i+1] == (i<3?109u:106u)); assert(consumer[i] == (i<3?1109u:1106u)); }
        assert(counters[0] == 0xabcdef01 && counters[65] == 0xabcdef01);
        for (int draw = 0; draw < 3; draw++) for (int i = 0; i < 64; i++) {
            uint8_t want[4] = {21*(draw+1),0,0,255}; assert(!memcmp(pixels[draw]+4*i, want, 4));
        }
        mtl_pool_del(handles); mtl_reg_tex(970, NULL, NULL); mtl_reg_tex(971, NULL, NULL);
        mtl_tex_release(&image); mtl_tex_release(&sample); mtl_queue_forget(970);
    }
    mtl_tex_release(&output);
    puts("Shared vertex/fragment typed-buffer atomics, GPU-produced uniform handles, ordered updates and compute consumers passed");
}
#include "uniform_storage.inc"
#include "../../../tests/dlss_spatial_corpus.h"
#include "../../../tests/dlss_temporal_corpus.h"
#include "feedback_capture.inc"
#include "alpha.inc"
#include "logic.inc"
#include "split_handle.inc"
#include "driver_c2.inc"
#include "resource_shapes.inc"
// Run with the maximum supported width on a known fixed- or selectable-size
// driver. Verify both a legal quad exchange and a full-width exchange whose
// conditional atomics must never execute when the driver cannot run it.
static void native_warp_width(unsigned available) {
    assert(available >= 4 && available <= 128);
    for (unsigned test = 0; test < 2; test++) {
        uint32_t words[64];
        for (unsigned i = 0; i < 64; i++) words[i] = 100 + i;
        list_t list = {0};
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &warp_program[test], .stages = 0x20);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)words, .size = sizeof words);
        RECORD(OP_DISPATCH, p_dispatch, .x = 1, .y = 1, .z = 1);
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (unsigned i = 0; i < 32; i++) {
            int active = (!test || available >= 32) && !(i & 4);
            assert(words[i] == 100 + i + active);
            assert(words[32 + i] == (active ? 100 + (i ^ (test ? 16u : 3u)) : 132 + i));
        }
    }
    puts("Conditional native shuffles and atomic side effects match the device subgroup width");
}

int main(void) {
    if (getenv("DM_VK_TEST_DLSS")) { dlss_spatial_corpus(1); dlss_temporal_corpus(); return 0; }
    if (getenv("DM_VK_TEST_TF_CAPTURE")) { feedback_capture(); return 0; }
    if (getenv("DM_VK_TEST_LOGIC")) { logic_operations(); sparse_mrt_outputs(); mixed_integer_outputs(); return 0; }
    if (getenv("DM_VK_TEST_ALPHA_COVERAGE")) { alpha_coverage(&logic_program[0],1); alpha_coverage(&logic_program[0],4); return 0; }
    if (getenv("DM_VK_TEST_ALPHA")) { alpha_references(); return 0; }
    if (getenv("DM_VK_TEST_DIVISORS")) { instance_divisors(); return 0; }
    if (getenv("DM_VK_TEST_BLEND_CONSTANTS")) { blend_constants(); return 0; }
    if (getenv("DM_VK_TEST_STENCIL_FACES")) { stencil_faces(); return 0; }
    if (getenv("DM_VK_TEST_PACKED_STREAMS")) { packed_streams(); return 0; }
    if (getenv("DM_VK_TEST_RESTART")) { primitive_restart(); return 0; }
    if (getenv("DM_VK_TEST_INDIRECT_DRAWS")) { indirect_draws(); return 0; }
    if (getenv("DM_VK_TEST_BUFFER_IMAGE")) { guarded_texture_transfers(); compressed_transfers(); return 0; }
    if (getenv("DM_VK_TEST_TEXTURE_COPY")) { texture_copy_regions(); hdr_scaled_copies(); downsample_view(); depth_copy_filter(); depth_plane_copy(); return 0; }
    if (getenv("DM_VK_TEST_CLEAR_RECT")) { scissored_attachment_clears(); integer_attachment_clears(); texture_clear_regions(); depth_copies(); return 0; }
    if (getenv("DM_VK_TEST_RENDER_COND")) { assert(DM_GAME_RENDER_COND); conditional_rendering(); return 0; }
    if (getenv("DM_VK_TEST_MSAA")) { multisample_colors(); multisample_depth(); return 0; }
    if (getenv("DM_VK_TEST_COUNTERS")) { visibility_reports(); segment_timestamps(); counter_boundaries(); return 0; }
    if (getenv("DM_VK_TEST_VIEWPORT")) { queue_viewport_origin(); origin_facing(); return 0; }
    if (getenv("DM_VK_TEST_DISCARD")) { graphics_storage(); return 0; }
    if (getenv("DM_VK_TEST_IMAGE_ORDER")) { image_order(); return 0; }
    if (getenv("DM_VK_TEST_FEEDBACK")) { attachment_feedback(); layered_feedback(); return 0; }
    if (getenv("DM_VK_TEST_CUSTOM_BORDER")) { custom_borders(); return 0; }
    if (getenv("DM_VK_TEST_POLYGON_FILL")) { polygon_fill(&fill_program,replay_features(NULL,NULL).fillModeNonSolid && !getenv("DM_VK_NO_WIREFRAME")); return 0; }
    if (getenv("DM_VK_TEST_POLYGON_OFFSET")) { polygon_offset(&offset_program); return 0; }
    if (getenv("DM_VK_TEST_DEPTH_CLAMP")) { depth_clamp(); return 0; }
    if (getenv("DM_VK_TEST_DEPTH_RANGE")) { nested_depth_ranges(); return 0; }
    const char *width = getenv("DM_VK_TEST_WARP_WIDTH");
    if (width) { native_warp_width((unsigned)atoi(width)); return 0; }
    if (getenv("DM_VK_TEST_C0")) { compute_driver_constants(); draw_parameters(); return 0; }
    if (getenv("DM_VK_TEST_C2")) { driver_c2(); c2_sample(); return 0; }
    driver_c2(); c2_sample();
    if (getenv("DM_VK_TEST_SPLIT_HANDLES")) { split_handles(); return 0; }
    split_handles();
    if (getenv("DM_VK_TEST_UNIFORM_STORAGE")) { uniform_storage(); return 0; }
    uniform_storage();
    if (getenv("DM_VK_TEST_TEXEL_GRAPHICS")) { typed_graphics(); return 0; }
    typed_graphics();
    if (getenv("DM_VK_TEST_TEXEL")) { typed_buffers(); return 0; }
    typed_buffers();
    if (getenv("DM_VK_TEST_DESCRIPTOR_POOLS")) { descriptor_lifetime(); return 0; }
    descriptor_lifetime();
    if(getenv("DM_VK_TEST_SEPARATE")) { separate_descriptors(0); separate_descriptors(1);return 0; }
    packed_streams();
    primitive_restart();
    indirect_draws();
    separate_descriptors(0); separate_descriptors(1);
    if(getenv("DM_VK_TEST_RETIRE")) { retired_texture_pool();return 0; }
    retired_texture_pool();
    const char *buffers=getenv("DM_VK_TEST_BUFFERS");
    if(buffers) { if(!strcmp(buffers,"pools")) pool_ranges();else if(!strcmp(buffers,"large")) buffer_range_lifetime();else simultaneous_aliases();return 0; }
    pool_ranges();
    if(getenv("DM_VK_TEST_HANDLES")) { uniform_texture_array(0,0); uniform_texture_array(1,0); uniform_texture_array(2,0);return 0; }
    gpu_indices();
    graphics_storage_alias();
    simultaneous_aliases();
    image_order();
    attachment_feedback();
    layered_feedback();
    graphics_storage();
    readback_gpu_consumer();
    buffer_range_lifetime();
    ordered_buffers();
    draw_parameters();
    texture_copy_regions(); hdr_scaled_copies(); downsample_view(); depth_copy_filter(); depth_plane_copy(); depth_copies(); stencil_faces(); blend_constants(); instance_divisors();
    clear_and_readback(NVN_FORMAT_RGBA8);
    clear_and_readback(NVN_FORMAT_RGBA8_SRGB);
    uploaded_texture(NVN_FORMAT_RGBA8);
    if (replay_features(NULL,NULL).textureCompressionASTC_LDR) uploaded_texture(NVN_FORMAT_RGBA_ASTC_4x4);
    else puts("ASTC upload sampling skipped: device lacks textureCompressionASTC_LDR");
    initial_texture_content();
    uniform_texture_array(0,STOCK_BORDER); uniform_texture_array(1,STOCK_BORDER); uniform_texture_array(2,STOCK_BORDER);
    dlss_spatial_corpus(1); dlss_temporal_corpus();
    feedback_capture();
    custom_borders();
    compute_driver_constants();
    many_resource_bindings();
    vertex_output_defaults();
    textured_draw(0, 0);  // CPU argument block, followed by a fully GPU-generated block
    textured_draw(1, 0);  // X is GPU-generated, Y and Z are CPU-written
    textured_draw(0, 1);  // argument producer and consumer in successive submissions
    vertex_fetch();
    registered_images();
    strided_layers();
    guarded_texture_transfers();
    compressed_transfers();
    queue_lifetime();
    origin_facing();
    queue_viewport_origin();
    nested_depth_ranges();
    depth_clamp();
    polygon_offset(&offset_program);
    polygon_fill(&fill_program,replay_features(NULL,NULL).fillModeNonSolid && !getenv("DM_VK_NO_WIREFRAME"));
    alpha_references();
    logic_operations();
    sparse_mrt_outputs();
    mixed_integer_outputs();
    alpha_coverage(&logic_program[0],1); alpha_coverage(&logic_program[0],4);
    scissored_attachment_clears();
    conditional_rendering();
    integer_attachment_clears();
    texture_clear_regions();
    multisample_colors();
    multisample_depth();
    visibility_reports();
    segment_timestamps(); counter_boundaries();
    puts("NVN compute image writes, cube-sampled draw, full/compact bindings and fenced Vulkan readback passed");
}
