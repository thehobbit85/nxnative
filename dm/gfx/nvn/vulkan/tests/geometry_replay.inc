static void geometry_replay(void) {
    tex_t target={.serial=0x17980,.d={.width=4,.height=4,.depth=1,.levels=1,
        .format=NVN_FORMAT_RGBA8,.target=NVN_TEXTURE_TARGET_2D}};
    nvn_tex_layout(&target.d,&target.L);mtl_queue_activate(target.serial);
    const float triangle[][4]={{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}};
    float vertices[195][4];uint16_t indices[195];
    for (unsigned i=0;i<195;i++) { memcpy(vertices[i],triangle[i%3],16);indices[i]=i%3; }
    mtl_pool_add(indices,sizeof indices);
    for (unsigned mode=0;mode<6;mode++) {
        uint8_t pixels[4*4*4];
        list_t list={0};
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,4,4}});
        RECORD(OP_SCISSOR,p_scissor,.w=4,.h=4);
        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0,0,1,1});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&geometry_program,.stages=7);
        p_bind_vattr attributes={.n=1};va_st attribute={.fmt=NVN_FORMAT_RGBA32F};
        memcpy(attributes.st,&attribute,sizeof attribute);put(&list,OP_BIND_VATTR,&attributes,sizeof attributes);
        RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=1,.st={{.stride=16}});
        RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=sizeof vertices,.addr=(uintptr_t)vertices);
        uint32_t arguments[]={mode<2 ? 3u : 195u,mode<4 ? 1u : 2u,0,0,7};
        mtl_pool_add(arguments,sizeof arguments);
        if (mode&1) {
            RECORD(OP_DRAW_IDX,p_draw_idx,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=arguments[0],.itype=1,
                .ibuf=(uintptr_t)indices,.instances=arguments[1],.base_instance=7,.ind=mode==5 ? (uintptr_t)arguments : 0);
        } else {
            arguments[3]=7;
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=arguments[0],.instances=arguments[1],
                .base_instance=7,.ind=mode==4 ? (uintptr_t)arguments : 0);
        }
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        mtl_pool_del(arguments);
        for (unsigned i=0;i<16;i++) if (memcmp(pixels+4*i,(uint8_t[]){255,0,0,255},4))
            die("geometry passthrough mode%u pixel%u: %u,%u,%u,%u",mode,i,pixels[4*i],pixels[4*i+1],pixels[4*i+2],pixels[4*i+3]);
    }
    mtl_pool_del(indices);mtl_tex_release(&target);mtl_queue_forget(target.serial);
    puts("Maxwell geometry capture, compute and raster replay passed direct/indexed/indirect instances and multi-block compaction");
}
