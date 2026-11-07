// The same encoded midtone must sample differently through a linear and an
// sRGB registration. Alpha is linear in both views. All blocks are solid.
static void compressed_color_views(void) {
    const struct { NVNformat linear, srgb; unsigned bytes; uint8_t block[16], rgba[4]; } cases[] = {
        {NVN_FORMAT_RGB_DXT1, NVN_FORMAT_RGB_DXT1_SRGB, 8, {0xa0,0xfa}, {255,85,0,255}},
        {NVN_FORMAT_RGBA_DXT1, NVN_FORMAT_RGBA_DXT1_SRGB, 8, {0xa0,0xfa}, {255,85,0,255}},
        {NVN_FORMAT_RGBA_DXT3, NVN_FORMAT_RGBA_DXT3_SRGB, 16,
         {0x99,0x99,0x99,0x99,0x99,0x99,0x99,0x99,0xa0,0xfa}, {255,85,0,153}},
        {NVN_FORMAT_RGBA_DXT5, NVN_FORMAT_RGBA_DXT5_SRGB, 16, {153,0,0,0,0,0,0,0,0xa0,0xfa}, {255,85,0,153}},
        // BC7 mode 6: equal RGBA endpoints with both endpoint p-bits set.
        {NVN_FORMAT_BPTC_UNORM, NVN_FORMAT_BPTC_UNORM_SRGB, 16, {0xc0,0xff,0x5f,0xa5,2,0,0x98,0xcc,1}, {255,85,1,153}}
    };
    enum { slot = 1460 };
    mtl_queue_activate(slot);
    mtl_reg_samp(slot, &(samp_par){.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE, .wt=NVN_WRAP_MODE_CLAMP_TO_EDGE});
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) for (int built_srgb=0; built_srgb<2; built_srgb++) {
        const typeof(cases[0]) *c = &cases[i];
        tex_t tex = {.serial=0x17200+2*i+built_srgb, .has_layout=1, .d={.target=NVN_TEXTURE_TARGET_2D_ARRAY,
                     .format=built_srgb ? c->srgb : c->linear, .width=8, .height=8, .depth=2, .levels=2}};
        nvn_tex_layout(&tex.d, &tex.L); tex.mem = calloc(1, tex.L.size); assert(tex.mem);
        assert(!nvn_swizzle(&tex.d, 1, 1, c->block, c->bytes, tex.mem));
        view_t view = {.set_target=1, .target=NVN_TEXTURE_TARGET_2D, .set_levels=1, .base_level=1, .nlevels=1,
                       .set_layers=1, .min_layer=1, .nlayers=1, .set_format=1};
        for (int pass=0; pass<3; pass++) {
            view.format = pass == 0 ? c->linear : pass == 1 ? c->srgb : NVN_FORMAT_RGBA32UI;
            mtl_reg_tex(slot, &tex, &view);
            list_t list={0};
            struct { uint32_t integers[8]; float rgba[4]; } result={0};
            RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&typed_border_program[0], .stages=0x20);
            RECORD(OP_BIND_TEX1, p_bind_tex1, .stage=5|BIND_TEX1_NOBASE, .slot=2, .h=((uint64_t)slot << 20) | slot);
            RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)&result, .size=sizeof result);
            RECORD(OP_DISPATCH, p_dispatch, .x=1, .y=1, .z=1);
            put(&list, OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
            for (int j=0; j<4; j++) {
                float f = c->rgba[j] / 255.f;
                if (pass == 1 && j < 3) f = f <= .04045f ? f/12.92f : powf((f+.055f)/1.055f, 2.4f);
                if (pass == 2) f = j == 3;
                if (!(fabsf(result.rgba[j]-f) < .002f))
                    die("BC base %#x view %#x channel %d: got %g expected %g", tex.d.format, view.format, j, result.rgba[j], f);
            }
        }
        mtl_reg_tex(slot, NULL, NULL); mtl_tex_release(&tex); free(tex.mem);
    }
    mtl_queue_forget(slot);
    puts("Decoded BC1/2/3/7 views select sRGB per registration and preserve linear alpha");
}
