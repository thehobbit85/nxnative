// Included by t_replay.c: reuse its real record writer and storage shaders.
static void uniform_storage(void) {
    enum { MODES = 5, CASES = 2 * MODES };
    uint32_t storage[CASES][68], uniform[CASES][8] = {{0}}, result[CASES][65] = {{0}};
    tex_t target = {.serial = 0xfa00, .d = {.width = 8, .height = 8, .depth = 1, .levels = 1,
                    .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&target.d, &target.L);
    for (int split = 0; split < 2; split++) for (int mode = 0; mode < MODES; mode++) {
        int c = split * MODES + mode;
        for (int i = 0; i < 68; i++) storage[c][i] = 10;
        uint64_t addresses[2] = {(uintptr_t)(storage[c]+1), (uintptr_t)(storage[c]+2)};
        uint32_t words[4]; memcpy(words, addresses, sizeof words);
        mtl_pool_add(storage[c], sizeof storage[c]); mtl_pool_add(uniform[c], sizeof uniform[c]);
        list_t list = {0};
        if (mode == 0) memcpy(uniform[c]+3, addresses, sizeof addresses);
        else if (mode == 3) {
            // Only the high halves are GPU-written. A four-byte preflight
            // would see no overlap and try to resolve stale truncated pointers.
            uniform[c][3] = words[0]; uniform[c][5] = words[2];
            for (int part = 0; part < 2; part++) {
                uint32_t high = words[2*part+1]; push_constants(&list, 1, 240, &high, sizeof high);
                RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &groups, .stages = 0x20);
                RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)(uniform[c]+4+2*part), .size = 4);
                RECORD(OP_DISPATCH, p_dispatch, .x = 1, .y = 1, .z = 1);
            }
        } else if (mode == 1 || mode == 4) {
            uint32_t values[4] = {0}; if (mode == 1) memcpy(values, words, sizeof values);
            push_constants(&list, 1, 240, values, sizeof values);
            RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &groups, .stages = 0x20);
            RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)(uniform[c]+3), .size = 16);
            RECORD(OP_DISPATCH, p_dispatch, .x = 4, .y = 1, .z = 1);
        }
        if (mode == 2 || mode == 4) {
            struct { uint64_t addr, size; uint32_t values[4]; } update = {(uintptr_t)(uniform[c]+3), 16, {0}};
            memcpy(update.values, words, sizeof words); put(&list, OP_UPDATE_UB, &update, sizeof update);
        }
        if (split) {
            put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); mtl_sub_commit(); list.length = 0;
        }
        uint32_t expected = 10; push_constants(&list, 0, 240, &expected, sizeof expected);
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&target});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &uniform_storage_program, .stages = 3);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 0, .index = 0, .addr = (uintptr_t)uniform[c], .size = sizeof uniform[c]);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = 1, .index = 0, .addr = (uintptr_t)uniform[c], .size = sizeof uniform[c]);
        RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &buffer_program, .stages = 0x20);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)(storage[c]+1), .size = 260);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 1, .addr = (uintptr_t)result[c], .size = sizeof result[c]);
        RECORD(OP_DISPATCH, p_dispatch, .x = 65, .y = 1, .z = 1);
        put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int i = 1; i <= 65; i++) {
            // Same oracle as graphics_storage_alias, using direct slot binds.
            uint32_t want = i == 2 || i == 3 ? 12 : 11;
            if (storage[c][i] != want || result[c][i-1] != want+1000) {
                fprintf(stderr, "Uniform storage mode%d split%d word%d: %u/%u expected %u/%u\n",
                        mode, split, i, storage[c][i], result[c][i-1], want, want+1000); abort();
            }
        }
        assert(storage[c][0] == 10 && storage[c][66] == 10 && storage[c][67] == 10);
        assert(uniform[c][0] == 0 && uniform[c][1] == 0 && uniform[c][2] == 0 && uniform[c][7] == 0);
        mtl_pool_del(uniform[c]); mtl_pool_del(storage[c]);
    }
    mtl_tex_release(&target);
    puts("Uniform storage addresses: CPU, GPU, high-half and ordered inline producers; alias atomics and consumers passed in 10 cases");
}
