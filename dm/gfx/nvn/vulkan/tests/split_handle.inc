static void split_handles(void) {
    enum { CASES = 180 };
    uint32_t first[CASES][8] = {{0}}, second[CASES][8] = {{0}};
    tex_t source = {.serial = 0xfb00, .d = {.width = 1, .height = 1, .depth = 1, .levels = 1,
                    .target = NVN_TEXTURE_TARGET_3D, .format = NVN_FORMAT_RGBA8}};
    tex_t output = {.serial = 0xfb01, .d = {.width = 4, .height = 4, .depth = 1, .levels = 1,
                    .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&source.d, &source.L); nvn_tex_layout(&output.d, &output.L);
    mtl_reg_tex(2053, &source, NULL);
    samp_par border = {.ws = NVN_WRAP_MODE_CLAMP_TO_BORDER, .border = {1,1,1,1}};
    mtl_reg_samp(515, &border);
    int c = 0;
    for (int family = 0; family < 3; family++) for (int producer = 0; producer < 3; producer++)
    for (int split = 0; split < 2; split++) for (int mode = 0; mode < 5; mode++) for (int outside = 0; outside < 2; outside++, c++) {
        assert(c < CASES);
        // Both orders, split ID bits, or the entire handle in either word.
        const uint32_t left[] = {5, 3u<<20, 1|(1u<<20), 0, 5|(3u<<20)};
        const uint32_t right[] = {3u<<20, 5, 4|(2u<<20), 5|(3u<<20), 0};
        mtl_pool_add(first[c], sizeof first[c]); mtl_pool_add(second[c], sizeof second[c]);
        uint32_t *dest[] = {first[c]+3, second[c]+5};
        const uint32_t value[] = {left[mode], right[mode]};
        list_t list = {0};
        for (int part = 0; part < 2; part++) {
            // A zero half remains CPU-owned: the fixed-sampler family must
            // preflight the texture's other word without a sampler read helping.
            if (!producer || !value[part]) *dest[part] = value[part];
            else if (producer == 1) {
                push_constants(&list, 1, 240, &value[part], sizeof value[part]);
                RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &groups, .stages = 0x20);
                RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)dest[part], .size = 4);
                RECORD(OP_DISPATCH, p_dispatch, .x = 1, .y = 1, .z = 1);
            } else {
                struct { uint64_t addr, size; uint32_t value, pad; } update = {(uintptr_t)dest[part], 4, value[part], 0};
                put(&list, OP_UPDATE_UB, &update, sizeof update);
            }
        }
        if (split) {
            put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); mtl_sub_commit(); list.length = 0;
        }
        float coordinate = outside ? 1.5f : 0.5f; uint8_t pixels[64] = {0};
        RECORD(OP_CLEAR_TEX, p_clear_tex, .tex = &source, .r = {.width = 1, .height = 1, .depth = 1}, .mask = 15, .c.f = {0,1,0,1});
        RECORD(OP_SET_TEX_POOL, p_set_pool, .pool = &texture_pool);
        RECORD(OP_SET_SAMP_POOL, p_set_pool, .pool = &alternate_sampler_pool);
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &split_program[family], .stages = 3);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 1, .index = 7, .addr = (uintptr_t)first[c], .size = sizeof first[c]);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 1, .index = 8, .addr = (uintptr_t)second[c], .size = sizeof second[c]);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 1, .index = 9, .addr = (uintptr_t)&coordinate, .size = sizeof coordinate);
        RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)pixels, .r = {.width = 4, .height = 4, .depth = 1});
        put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        const uint8_t expected[] = {outside ? 255 : 0, 255, outside ? 255 : 0, 255};
        for (int pixel = 0; pixel < 16; pixel++) if (memcmp(pixels+4*pixel, expected, sizeof expected)) {
            fprintf(stderr, "Split handle family%d producer%d split%d mode%d outside%d pixel%d: %u,%u,%u,%u\n",
                    family, producer, split, mode, outside, pixel, pixels[4*pixel], pixels[4*pixel+1], pixels[4*pixel+2], pixels[4*pixel+3]); abort();
        }
        for (int i = 0; i < 8; i++) { assert(first[c][i] == (i == 3 ? value[0] : 0)); assert(second[c][i] == (i == 5 ? value[1] : 0)); }
        mtl_pool_del(first[c]); mtl_pool_del(second[c]);
    }
    assert(c == CASES);
    list_t list = {0};
    RECORD(OP_SET_TEX_POOL, p_set_pool, .pool = NULL); RECORD(OP_SET_SAMP_POOL, p_set_pool, .pool = NULL);
    put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    mtl_reg_tex(2053, NULL, NULL); mtl_tex_release(&source); mtl_tex_release(&output);
    puts("Split handles: 180 combined/separate/fixed-sampler, CPU/GPU/inline, same/split submission, pool-base and sampler-border cases passed");
}
