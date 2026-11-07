static void mesh_replay(void) {
    tex_t target={.serial=0x17ab0,.d={.width=4,.height=4,.depth=1,.levels=1,
        .format=NVN_FORMAT_RGBA8,.target=NVN_TEXTURE_TARGET_2D}};
    nvn_tex_layout(&target.d,&target.L);mtl_queue_activate(target.serial);
    const uint32_t counts[]={0,1,2,3,65,300};
    for (unsigned indirect=0;indirect<2;indirect++) for (unsigned i=0;i<sizeof counts/sizeof *counts;i++) {
        uint32_t arguments[]={counts[i],7},blue=43+i;
        uint8_t pixels[64];
        mtl_pool_add(arguments,sizeof arguments);
        list_t list={0};
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,4,4}});
        RECORD(OP_SCISSOR,p_scissor,.w=4,.h=4);
        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0,0,0,1});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&mesh_program,.stages=(1u<<1)|(1u<<4));
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=4,.index=0,.addr=(uintptr_t)&blue,.size=4);
        RECORD(OP_DRAW,p_draw,.prim=DM_PRIM_MESH_TASKS,.first=7,.count=counts[i],.ind=indirect ? (uintptr_t)arguments : 0);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        unsigned last=counts[i] ? 7+counts[i]-1 : 0;
        if (last && last%3==0) last--;
        uint8_t expected[]={last ? last%251 : 0,last ? 2 : 0,last ? blue : 0,255};
        for (unsigned pixel=0;pixel<16;pixel++) if (memcmp(pixels+pixel*4,expected,4))
            die("mesh raster indirect%u count%u pixel%u got %u,%u,%u,%u expected %u,%u,%u,%u",indirect,counts[i],pixel,
                pixels[pixel*4],pixels[pixel*4+1],pixels[pixel*4+2],pixels[pixel*4+3],expected[0],expected[1],expected[2],expected[3]);
        mtl_pool_del(arguments);
    }
    mtl_tex_release(&target);mtl_queue_forget(target.serial);
    puts("Mesh replay passed direct/indirect group IDs, empty groups, multi-block compaction, per-primitive attributes and stage-four uniforms");
}
