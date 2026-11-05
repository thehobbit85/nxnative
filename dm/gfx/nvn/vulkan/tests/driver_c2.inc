static void driver_c2(void) {
    uint32_t results[4][1026];
    for (int pass = 0; pass < 4; pass++) {
        uint32_t expected[1024] = {0};
        for (int i = 0; i < 1026; i++) results[pass][i] = 0xcafebabe;
        list_t list = {0};
        const int slots[] = {0,31,32,34,39,40,41,128,255};
        for (size_t i = 0; i < sizeof slots/sizeof *slots; i++) {
            int slot = slots[i]; uint32_t value = 1000+100*pass+slot;
            RECORD(OP_BIND_TEX1, p_bind_tex1, .stage = 5|BIND_TEX1_NOBASE, .slot = slot, .h = value);
            if (slot <= 40) expected[(0x20+8*slot)/4] = value;
            else if (slot == 128) expected[0x168/4] = value;
            else if (slot == 255) expected[0x560/4] = value;
        }
        for (int slot = 0; slot < 8; slot++) {
            uint32_t value = pass&1 ? 2000+slot : 0;
            RECORD(OP_BIND_IMG, p_bind_handles, .stage = 5, .first = slot, .n = 1, .h = {value});
            if (value) expected[(0x120+8*slot)/4] = value;
        }
        for (int slot = 0; slot < 128; slot += 127) {
            uint32_t value = (uint32_t)(300+pass+slot)<<20;
            RECORD(OP_BIND_SAMP, p_bind_handles, .stage = 5, .first = slot, .n = 1, .h = {value});
            expected[(0x568+8*slot)/4] = value;
        }
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &driver_c2_program, .stages = 0x20);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage = 5, .index = 0, .addr = (uintptr_t)(results[pass]+1), .size = sizeof expected);
        RECORD(OP_DISPATCH, p_dispatch, .x = 16, .y = 1, .z = 1);
        put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        for (int i = 0; i < 1024; i++) if (results[pass][i+1] != expected[i]) {
            fprintf(stderr, "C2 table pass%d byte%#x: %#x expected %#x\n", pass, i*4, results[pass][i+1], expected[i]); abort();
        }
        assert(results[pass][0] == 0xcafebabe && results[pass][1025] == 0xcafebabe);
    }
    list_t list = {0};
    const int slots[] = {0,31,32,34,39,40,41,128,255};
    for (size_t i = 0; i < sizeof slots/sizeof *slots; i++) {
        RECORD(OP_BIND_TEX1, p_bind_tex1, .stage = 5|BIND_TEX1_NOBASE, .slot = slots[i], .h = 0);
    }
    for (int slot = 0; slot < 8; slot++) { RECORD(OP_BIND_IMG, p_bind_handles, .stage = 5, .first = slot, .n = 1, .h = {0}); }
    for (int slot = 0; slot < 128; slot += 127) { RECORD(OP_BIND_SAMP, p_bind_handles, .stage = 5, .first = slot, .n = 1, .h = {0}); }
    put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    puts("C2 driver bytes preserve high combined slots, image priority/unbind fallback, table endpoints and zero padding");
}

static void c2_sample(void) {
    tex_t source[2], output = {.serial = 0xfc02, .d = {.width = 4, .height = 4, .depth = 1, .levels = 1,
                               .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    for (int i = 0; i < 2; i++) { source[i] = output; source[i].serial = 0xfc00+i; mtl_reg_tex(980+i, &source[i], NULL); }
    samp_par nearest = {0}; mtl_reg_samp(980, &nearest);
    for (int pass = 0; pass < 3; pass++) {
        uint8_t pixels[64] = {0}; list_t list = {0};
        for (int i = 0; i < 2; i++) {
            p_clear_tex clear = {.tex = &source[i], .r = {.width = 4, .height = 4, .depth = 1}, .mask = 15};
            clear.c.f[i+1] = 1; clear.c.f[3] = 1; put(&list, OP_CLEAR_TEX, &clear, sizeof clear);
        }
        RECORD(OP_BIND_TEX1, p_bind_tex1, .stage = 1|BIND_TEX1_NOBASE, .slot = 34, .h = 980);
        RECORD(OP_BIND_IMG, p_bind_handles, .stage = 1, .first = 2, .n = 1, .h = {pass == 1 ? 981 : 0});
        RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = &c2_sample_program, .stages = 3);
        RECORD(OP_DRAW, p_draw, .prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex = &output, .src = (uintptr_t)pixels, .r = {.width = 4, .height = 4, .depth = 1});
        put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        const uint8_t expected[] = {0, pass == 1 ? 0 : 255, pass == 1 ? 255 : 0, 255};
        for (int i = 0; i < 16; i++) assert(!memcmp(pixels+4*i, expected, sizeof expected));
    }
    for (int i = 0; i < 2; i++) { mtl_reg_tex(980+i, NULL, NULL); mtl_tex_release(&source[i]); }
    mtl_tex_release(&output);
    puts("High-slot texture sampling follows image priority and falls back after image unbind");
}
