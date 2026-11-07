// The shader reads a packed four-byte texel as one 32-bit register and writes
// different words through two disjoint mip/layer views of the same allocation.
static void storage_image_views(void) {
    enum { slot = 1450, layers = 4, words = 4 * layers };
    const NVNformat formats[] = {NVN_FORMAT_RGBA8, NVN_FORMAT_R32F, NVN_FORMAT_RG16UI};
    mtl_queue_activate(slot);
    for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) for (int formatless = 0; formatless < 2; formatless++) {
        tex_t t = {.serial=0x17170+2*i+formatless, .d={.width=4, .height=4, .depth=layers, .levels=2,
                   .format=formats[i], .target=NVN_TEXTURE_TARGET_2D_ARRAY, .flags=NVN_TEXTURE_FLAGS_IMAGE}};
        nvn_tex_layout(&t.d, &t.L);
        view_t mip = {.set_levels=1, .base_level=1, .nlevels=1};
        view_t view = mip;
        view.set_layers = 1; view.min_layer = 1; view.nlayers = 1;
        view.set_swizzle = 1; memcpy(view.swz, (uint8_t[]){0, 0, 0, 0}, 4); // Storage ignores sampled swizzles.
        mtl_reg_tex(slot, &t, &view);
        view.min_layer = 2; view.nlayers = 2;
        mtl_reg_tex(slot+1, &t, &view);
        uint32_t source[words], dest[words], previous[4] = {0};
        for (int j = 0; j < words; j++) source[j] = 0x82a41937u + 0x01010101u * j;
        list_t list = {0};
        RECORD(OP_COPY_B2T, p_copy_b2t, .tex=&t, .has_view=1, .view=mip, .src=(uintptr_t)source,
               .r={.width=2, .height=2, .depth=layers});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&storage_views_program[formatless], .stages=0x20);
        RECORD(OP_BIND_IMG, p_bind_handles, .stage=5, .first=0, .n=2, .h={slot, slot+1});
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)previous, .size=sizeof previous);
        RECORD(OP_DISPATCH, p_dispatch, .x=1, .y=1, .z=1);
        RECORD(OP_COPY_T2B, p_copy_b2t, .tex=&t, .has_view=1, .view=mip, .src=(uintptr_t)dest,
               .r={.width=2, .height=2, .depth=layers});
        put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        if (memcmp(previous, (uint32_t[]){formatless ? 0 : source[5], formatless ? 0 : source[14], 2, 2}, sizeof previous))
            die("storage views format %#x: loaded %#x/%#x expected %#x/%#x, dimensions %u/%u expected 2/2",
                formats[i], previous[0], previous[1], source[5], source[14], previous[2], previous[3]);
        source[5] = formatless ? (formats[i] == NVN_FORMAT_RG16UI ? 0x189035a2u : 0x35a2u) : 0x91b3d572u;
        source[14] = (uint32_t)-1875142321;
        assert(!memcmp(dest, source, sizeof dest));
        mtl_reg_tex(slot, NULL, NULL); mtl_reg_tex(slot+1, NULL, NULL); mtl_tex_release(&t);
    }
    mtl_queue_forget(slot);
    puts("Signed and unsigned storage views preserve packed texel words, dimensions and adjacent texels");
}
