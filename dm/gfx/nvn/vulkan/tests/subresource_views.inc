static void subresource_views(void) {
    enum { slot = 1504, base_words = 8*8*8, mip_words = 4*4*4 };
    for (int volume = 0; volume < 2; volume++) {
        tex_t texture = {.serial=0x17380+volume, .d={.width=8, .height=volume ? 8 : 4,
            .depth=volume ? 8 : 1, .levels=2, .format=NVN_FORMAT_R32UI,
            .target=volume ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_1D_ARRAY}};
        nvn_tex_layout(&texture.d, &texture.L);
        view_t mip = {.set_levels=1, .base_level=1, .nlevels=1};
        view_t selected = mip;
        if (!volume) { selected.set_layers=1; selected.min_layer=1; selected.nlayers=2; }
        mtl_reg_tex(slot, &texture, &selected);
        mtl_queue_activate(slot);
        for (int pass = 0; pass < 3; pass++) {
            uint32_t base[base_words], source[mip_words], returned[base_words], result[8];
            unsigned base_count = volume ? base_words : 8*4, mip_count = volume ? mip_words : 4*4;
            for (unsigned i=0; i<base_count; i++) base[i] = 0xa0000000u + i;
            for (unsigned i=0; i<mip_count; i++) source[i] = 0xb0000000u + i;
            memset(result, 0xcc, sizeof result);
            list_t list = {0};
            RECORD(OP_COPY_B2T, p_copy_b2t, .tex=&texture, .src=(uintptr_t)base,
                .r={.width=8, .height=volume ? 8 : 1, .depth=volume ? 8 : 4});
            RECORD(OP_COPY_B2T, p_copy_b2t, .tex=&texture, .has_view=1, .view=mip, .src=(uintptr_t)source,
                .r={.width=4, .height=volume ? 4 : 1, .depth=4});
            uint32_t expected[8] = {source[5], source[10], 4, 2, 4, 1, 4, 2};
            if (volume) {
                view_t slice = mip;
                slice.set_layers=1; slice.min_layer=pass+1; slice.nlayers=1;
                RECORD(OP_SET_RT, p_set_rt, .n=1, .color={&texture}, .has_cview={1}, .cview={slice});
                RECORD(OP_SCISSOR, p_scissor, .x=1, .y=1, .w=2, .h=2);
                p_clear_color clear = {.mask=15, .rgba={1000.0f+pass}};
                put(&list, OP_CLEAR_COLOR, &clear, sizeof clear);
                for (int y=1; y<=2; y++) for (int x=1; x<=2; x++)
                    source[(pass+1)*16+y*4+x] = 1000+pass;
                memcpy(expected, (uint32_t[]){source[5], source[21], source[37], source[53], 4, 4, 4, 1}, sizeof expected);
            }
            RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&subresource_program[volume], .stages=0x20);
            RECORD(OP_BIND_TEX, p_bind_handles, .stage=5, .first=0, .n=2, .h={slot, slot});
            RECORD(OP_BIND_IMG, p_bind_handles, .stage=5, .first=0, .n=1, .h={slot});
            RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)result, .size=sizeof result);
            RECORD(OP_DISPATCH, p_dispatch, .x=1, .y=1, .z=1);
            RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&texture, .has_view=1, .view=mip, .src=(uintptr_t)returned,
                .r={.width=4, .height=volume ? 4 : 1, .depth=4});
            put(&list, OP_END, NULL, 0);
            mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
            for (int i=0; i<8; i++) if (result[i] != expected[i])
                die("subresource volume%d pass%d result%d: %#x expected %#x", volume, pass, i, result[i], expected[i]);
            source[volume ? 3*16+2*4+2 : 2*4+2] = 0x91827364u;
            for (unsigned i=0; i<mip_count; i++) if (returned[i] != source[i])
                die("subresource volume%d pass%d texel%u: %#x expected %#x", volume, pass, i, returned[i], source[i]);
            list = (list_t){0};
            RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&texture, .src=(uintptr_t)returned,
                .r={.width=8, .height=volume ? 8 : 1, .depth=volume ? 8 : 4});
            put(&list, OP_END, NULL, 0);
            mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
            assert(!memcmp(base, returned, base_count * sizeof *base));
        }
        mtl_reg_tex(slot, NULL, NULL); mtl_tex_release(&texture); mtl_queue_forget(slot);
    }
    puts("1D array and volume mip views preserve sampled/storage regions, 3D attachments and every untouched texel");
}
