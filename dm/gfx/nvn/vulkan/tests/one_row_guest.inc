#include "../../../../android/tests/texture_coordinates_cases.h"

// The real Maxwell sampler exports a 2D interface, while its dimension query
// exports 1D. Execute both against the same guest texture and selected mip.
static void one_row_guest(void) {
    enum { slot = 1508 };
    uint32_t *uniform = calloc(16384, sizeof *uniform);
    uint32_t *output = malloc(256 * sizeof *output);
    assert(uniform && output);
    mtl_pool_add(uniform, 65536); mtl_pool_add(output, 1024);
    mtl_queue_activate(slot);
    unsigned checks = 0;
    for (int array = 0; array < 2; array++) {
        tex_t texture = {.serial=0x17480+array, .d={.width=8, .height=array ? 4 : 1,
            .depth=1, .levels=4, .format=NVN_FORMAT_RGBA32F,
            .target=array ? NVN_TEXTURE_TARGET_1D_ARRAY : NVN_TEXTURE_TARGET_1D}};
        nvn_tex_layout(&texture.d, &texture.L);
        float pixels[4][4][32]; list_t upload = {0};
        for (int mip = 0; mip < 4; mip++) for (int layer = 0; layer < (array ? 4 : 1); layer++) {
            tco_pixels(pixels[mip][layer], array, mip, layer);
            p_copy_b2t copy = {.tex=&texture, .src=(uintptr_t)pixels[mip][layer], .has_view=1,
                .view={.set_levels=1, .base_level=mip, .nlevels=1},
                .r={.width=8>>mip, .height=1, .zoffset=layer, .depth=1}};
            put(&upload, OP_COPY_B2T, &copy, sizeof copy);
        }
        put(&upload, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(upload.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (unsigned base = 0; base < 2; base++) {
            view_t view = {.set_levels=1, .base_level=base, .set_layers=array, .min_layer=array, .nlayers=3};
            mtl_reg_tex(slot, &texture, &view);
            for (unsigned test = 0; test <= tco_count(array); test++) {
                int query = test == tco_count(array);
                uint32_t expected[4];
                tco_inputs(array, base, query ? 0 : test, uniform, expected);
                if (query) memcpy(expected, (uint32_t[]){8>>base, array ? 3u : 0, 0, 4-base}, sizeof expected);
                uint64_t address = (uintptr_t)(output + 64);
                memcpy(uniform + 14, &address, sizeof address);
                memset(output, 0xcc, 1024);
                samp_par sampler = {.minf=NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST, .lod_max=16,
                    .ws=tco_wrap(test) ? NVN_WRAP_MODE_REPEAT : NVN_WRAP_MODE_CLAMP_TO_EDGE,
                    .wt=NVN_WRAP_MODE_CLAMP_TO_EDGE, .wr=NVN_WRAP_MODE_CLAMP_TO_EDGE};
                mtl_reg_samp(slot, &sampler);
                list_t list = {0};
                RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&one_row_guest_program[query][array], .stages=0x20);
                RECORD(OP_BIND_UBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)uniform, .size=65536);
                RECORD(OP_BIND_TEX1, p_bind_tex1, .stage=5|BIND_TEX1_NOBASE, .slot=128, .h=slot);
                RECORD(OP_BIND_SAMP, p_bind_handles, .stage=5, .first=1, .n=1, .h={(uint64_t)slot<<20});
                RECORD(OP_DISPATCH, p_dispatch, .x=1, .y=1, .z=1);
                put(&list, OP_END, NULL, 0);
                mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
                for (int word = 0; word < 256; word++) {
                    uint32_t want = word >= 64 && word < 68 ? expected[word-64] : 0xcccccccc;
                    if (output[word] != want)
                        die("Maxwell one-row array%d base%u case%u word%d: %#x expected %#x", array, base, test, word, output[word], want);
                }
                checks++;
            }
        }
        mtl_reg_tex(slot, NULL, NULL); mtl_tex_release(&texture);
    }
    mtl_queue_forget(slot); mtl_pool_del(uniform); mtl_pool_del(output); free(uniform); free(output);
    printf("Actual Maxwell 1D/array sampling and dimension queries passed %u selected-mip, layer and wrap cases\n", checks);
}
