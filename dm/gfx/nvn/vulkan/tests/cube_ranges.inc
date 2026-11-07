// Bind one guest view to three shader dimensions. Face values identify both
// the physical layer and mip; size queries also distinguish real and dummy views.
static void cube_ranges(void) {
    enum { layers = 18, slot = 1440 };
    tex_t tex = {.serial=0x17160, .d={.width=4, .height=4, .depth=layers, .levels=3,
                 .format=NVN_FORMAT_R32F, .target=NVN_TEXTURE_TARGET_2D_ARRAY}};
    nvn_tex_layout(&tex.d, &tex.L);
    mtl_queue_activate(slot);
    mtl_reg_samp(slot, &(samp_par){.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE, .wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,
                                   .wr=NVN_WRAP_MODE_CLAMP_TO_EDGE});
    for (int level = 0; level < 3; level++) {
        list_t list = {0};
        for (int layer = 0; layer < layers; layer++) {
            RECORD(OP_CLEAR_TEX, p_clear_tex, .tex=&tex, .has_view=1, .view={.set_levels=1, .base_level=level},
                   .r={.zoffset=layer, .width=4 >> level, .height=4 >> level, .depth=1},
                   .mask=15, .c.f={100 + 32 * level + layer});
        }
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    }
    const int array = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, cube = NVN_TEXTURE_TARGET_CUBEMAP;
    const struct { int target, base, count, mip, levels, first, faces, exposed_levels; } cases[] = {
        {array, 1, 2, 1, 0, 6, 12, 2}, {array, 6, 12, 1, 7, 6, 12, 2},
        {cube, 2, 1, 0, 1, 12, 6, 1}, {cube, 6, 12, 0, 1, 6, 6, 1},
        {array, 6, 0, 2, 1, 6, 12, 1}, {array, 0, 0, 0, 0, 0, 18, 3},
        {array, 14, 4, 1, 1, 14, 4, 1}, // Only the 2D-array slot can use these four faces.
        {NVN_TEXTURE_TARGET_2D, 8, 6, 0, 1, 8, 1, 1},
        {array, 18, 1, 0, 1, 0, 0, 1}, {array, 0, 6, 255, 1, 0, 0, 1},
    };
    for (unsigned row = 0; row < sizeof cases / sizeof cases[0]; row++) {
        const typeof(cases[0]) *c = &cases[row];
        view_t view = {.set_target=1, .target=c->target, .set_layers=1, .min_layer=c->base, .nlayers=c->count,
                       .set_levels=1, .base_level=c->mip, .nlevels=c->levels};
        mtl_reg_tex(slot, &tex, &view);
        int32_t result[layers+1][4]; memset(result, 0xcc, sizeof result);
        list_t list = {0};
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&cube_ranges_program, .stages=0x20);
        for (int i = 0; i < 3; i++)
            RECORD(OP_BIND_TEX1, p_bind_tex1, .stage=5|BIND_TEX1_NOBASE, .slot=i, .h=((uint64_t)slot << 20) | slot);
        RECORD(OP_BIND_SSBO, p_bind_buf, .stage=5, .index=0, .addr=(uintptr_t)result, .size=sizeof result);
        RECORD(OP_DISPATCH, p_dispatch, .x=layers, .y=1, .z=1);
        put(&list, OP_END, NULL, 0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        int width = c->faces ? 4 >> c->mip : 1, cubes = c->faces / 6;
        int dims[] = {cubes ? width : 1, cubes ? cubes : 1, c->faces ? c->faces : 1, width};
        if (memcmp(result[layers], dims, sizeof dims))
            die("cube ranges case %u dimensions %d/%d/%d/%d expected %d/%d/%d/%d", row, result[layers][0],
                result[layers][1], result[layers][2], result[layers][3], dims[0], dims[1], dims[2], dims[3]);
        int base = 100 + 32 * c->mip + c->first;
        for (int i = 0; i < layers; i++) {
            int face = i % 6, group = i / 6 < cubes ? i / 6 : cubes - 1;
            int expected[] = {cubes ? base + face : 0, cubes ? base + 6 * group + face : 0,
                              c->faces ? base + (i < c->faces ? i : c->faces-1) : 0, c->exposed_levels};
            for (int j = 0; j < 4; j++) if (result[i][j] != expected[j])
                die("cube ranges case %u face %d channel %d returned %d expected %d", row, i, j, result[i][j], expected[j]);
        }
    }
    mtl_reg_tex(slot, NULL, NULL); mtl_tex_release(&tex); mtl_queue_forget(slot);
    puts("Cube, cube-array and face-array descriptors preserve face, mip and range queries");
}
