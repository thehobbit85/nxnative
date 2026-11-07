static void empty_multisample_images(void) {
    if (!replay_features(NULL, NULL).shaderStorageImageMultisample) {
        puts("Multisample storage slots require shaderStorageImageMultisample");
        return;
    }
    enum { slot = 1501 };
    tex_t depth = {.serial=0x17371, .d={.width=1, .height=1, .depth=1, .levels=1,
        .format=NVN_FORMAT_DEPTH32F, .target=NVN_TEXTURE_TARGET_2D_MULTISAMPLE, .samples=4}};
    nvn_tex_layout(&depth.d, &depth.L);
    mtl_reg_tex(slot, &depth, NULL);
    tex_t ordinary = {.serial=0x17372, .d={.width=1, .height=1, .depth=1, .levels=1,
        .format=NVN_FORMAT_RGBA8, .target=NVN_TEXTURE_TARGET_2D}};
    nvn_tex_layout(&ordinary.d, &ordinary.L);
    mtl_reg_tex(slot+1, &ordinary, NULL);
    samp_par compare = {.cmp_mode=NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE, .cmp_func=NVN_COMPARE_FUNC_GREATER};
    mtl_reg_samp(slot, &compare);
    mtl_queue_activate(slot);
    for (int pass = 0; pass < 2; pass++) {
        uint32_t results[8][4];
        memset(results, 0xcc, sizeof results);
        list_t list = {0};
        RECORD(OP_CLEAR_TEX, p_clear_tex, .tex=&ordinary, .mask=15, .c.f={1, 1, 1, 1},
            .r={.width=1, .height=1, .depth=1});
        RECORD(OP_SET_RT, p_set_rt, .depth=&depth);
        RECORD(OP_CLEAR_DS, p_clear_ds, .depth=0.25f, .depth_mask=1);
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&empty_ms_program, .stages=0x20);
        RECORD(OP_BIND_IMG, p_bind_handles, .stage=5, .first=0, .n=2, .h={0, 0});
        RECORD(OP_BIND_TEX, p_bind_handles, .stage=5, .first=0, .n=5,
            .h={pass ? slot+1 : 0, 0, 0, slot, ((uint64_t)slot << 20) | (pass ? slot+1 : 0)});
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)results, .size=sizeof results);
        RECORD(OP_DISPATCH, p_dispatch, .x=1, .y=1, .z=1);
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        const uint32_t expected[8][4] = {{0}, {0}, {0}, {0x3e800000, 0, 0, 0x3f800000},
            {100, 100, 100, 100}, {4, 4, 4, 4}, {17, 18, 19, 20}, {41, 42, 43, 44}};
        for (int row = 0; row < 8; row++) for (int c = 0; c < 4; c++)
            if (results[row][c] != expected[row][c])
                die("multisample slot pass %d result %d/%d: %#x expected %#x", pass, row, c, results[row][c], expected[row][c]);
    }
    mtl_reg_tex(slot, NULL, NULL); mtl_tex_release(&depth); mtl_queue_forget(slot);
    mtl_reg_tex(slot+1, NULL, NULL); mtl_tex_release(&ordinary);
    puts("Multisample float/int/uint zeros, storage sinks, real depth and empty depth-compare slots passed");
}
