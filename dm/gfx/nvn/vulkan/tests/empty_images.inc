static void empty_storage_images(void) {
    enum { slot = 1500 };
    tex_t data = {.serial=0x17370, .d={.width=1, .height=1, .depth=1, .levels=1,
        .format=NVN_FORMAT_R32UI, .target=NVN_TEXTURE_TARGET_2D, .flags=NVN_TEXTURE_FLAGS_IMAGE}};
    nvn_tex_layout(&data.d, &data.L);
    mtl_reg_tex(slot, &data, NULL);
    mtl_queue_activate(slot);
    for (int pass = 0; pass < 4; pass++) {
        uint32_t original = 29, returned = 0, results[5];
        memset(results, 0xcc, sizeof results);
        uint64_t empty = pass & 1 ? 0xfffff : 0;
        list_t list = {0};
        RECORD(OP_COPY_B2T, p_copy_b2t, .tex=&data, .src=(uintptr_t)&original,
            .r={.width=1, .height=1, .depth=1});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&empty_images_program, .stages=0x20);
        RECORD(OP_BIND_IMG, p_bind_handles, .stage=5, .first=0, .n=6,
            .h={empty, slot, empty, empty, empty, empty});
        RECORD(OP_BIND_TEX1, p_bind_tex1, .stage=5 | BIND_TEX1_NOBASE, .slot=0, .h=0);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)results, .size=sizeof results);
        RECORD(OP_DISPATCH, p_dispatch, .x=1, .y=1, .z=1);
        RECORD(OP_DISPATCH, p_dispatch, .x=1, .y=1, .z=1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&data, .src=(uintptr_t)&returned,
            .r={.width=1, .height=1, .depth=1});
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        if (returned != original + 14 || memcmp(results, (uint32_t[]){0, 0, 0, 0, 123}, sizeof results))
            die("empty storage pass %d: live image %u expected %u; sampled %u/%u/%u/%u, reached %u",
                pass, returned, original+14, results[0], results[1], results[2], results[3], results[4]);
    }
    // A graphics pipeline whose only image is missing must still draw, and
    // subsequent draws must order writes to the same atomic sink.
    uint32_t first = 0, second = 0;
    for (int pass = 0; pass < 2; pass++) {
        list_t list = {0};
        RECORD(OP_SET_RT, p_set_rt, .n=1, .color={&data});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&image_draw, .stages=3);
        RECORD(OP_BIND_IMG, p_bind_handles, .stage=1, .first=3, .n=1, .h={0});
        for (int draw = 0; draw <= pass; draw++)
            RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .count=3, .instances=1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&data, .src=(uintptr_t)(pass ? &second : &first),
            .r={.width=1, .height=1, .depth=1});
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    }
    if (second != first + 6) die("unbound fragment atomic sink: %u then %u expected %u", first, second, first+6);
    mtl_reg_tex(slot, NULL, NULL); mtl_tex_release(&data); mtl_queue_forget(slot);
    puts("Unbound storage arrays, float and integer write sinks and atomics keep live work and sampled zeros intact");
}
