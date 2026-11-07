// A layer-writing vertex shader must reach every selected plane. Switching
// back to an ordinary shader must leave all but the first plane untouched.
static void layered_attachments(void) {
    enum { slot=1595, words=8*8*8 };
    const int targets[]={NVN_TEXTURE_TARGET_2D_ARRAY,NVN_TEXTURE_TARGET_3D,NVN_TEXTURE_TARGET_CUBEMAP};
    unsigned checks=0;
    mtl_queue_activate(slot);
    for (unsigned shape=0; shape<3; shape++) for (unsigned mip=0; mip<2; mip++)
      for (unsigned first=0; first<2; first++) for (unsigned depth_test=0; depth_test<2; depth_test++) {
        unsigned total=shape==2 ? 6 : shape==1 ? 8>>mip : 8, size=8>>mip;
        unsigned count=total-first;
        tex_t target={.serial=0x17900+shape,.d={.width=8,.height=8,.depth=shape==2 ? 1 : 8,
            .levels=2,.format=NVN_FORMAT_R32UI,.target=targets[shape]}};
        nvn_tex_layout(&target.d,&target.L);
        tex_t other=target;other.serial+=16;
        tex_t depth={.serial=0x17920+shape,.d={.width=8,.height=8,.depth=total,.levels=2,
            .format=NVN_FORMAT_DEPTH32F,.target=NVN_TEXTURE_TARGET_2D_ARRAY}};
        nvn_tex_layout(&depth.d,&depth.L);
        uint32_t initial[words],returned[words],second[words];
        float depths[words],returned_depths[words];
        for (unsigned i=0; i<words; i++) initial[i]=0x11223344;
        for (unsigned i=0; i<words; i++) depths[i]=(i/(size*size))%2 ? 1 : 0;
        view_t level={.set_levels=1,.base_level=mip,.nlevels=1};
        view_t view=level;view.set_layers=1;view.min_layer=first;view.nlayers=count;
        list_t list={0};
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&target,.has_view=1,.view=level,.src=(uintptr_t)initial,
            .r={.width=size,.height=size,.depth=total});
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&other,.has_view=1,.view=level,.src=(uintptr_t)initial,
            .r={.width=size,.height=size,.depth=total});
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&depth,.has_view=1,.view=level,.src=(uintptr_t)depths,
            .r={.width=size,.height=size,.depth=total});
        RECORD(OP_SET_RT,p_set_rt,.n=2,.color={&target,&other},.has_cview={1,1},.cview={view,view},
            .depth=depth_test ? &depth : NULL,.has_dview=1,.dview=view);
        p_bind_state state={.kind=5};
        ds_st ds={.dt=depth_test,.dw=depth_test,.df=NVN_COMPARE_FUNC_LEQUAL};
        memcpy(state.bytes,&ds,sizeof ds);put(&list,OP_BIND_STATE,&state,sizeof state);
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,size,size}});
        RECORD(OP_SCISSOR,p_scissor,.x=1,.y=1,.w=size-2,.h=size-2);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&layered_program[0],.stages=3);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=count);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&layered_program[1],.stages=3);
        RECORD(OP_SCISSOR,p_scissor,.x=1,.y=1,.w=1,.h=1);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1,.base_instance=9);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.has_view=1,.view=level,.src=(uintptr_t)returned,
            .r={.width=size,.height=size,.depth=total});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&other,.has_view=1,.view=level,.src=(uintptr_t)second,
            .r={.width=size,.height=size,.depth=total});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&depth,.has_view=1,.view=level,.src=(uintptr_t)returned_depths,
            .r={.width=size,.height=size,.depth=total});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (unsigned layer=0; layer<total; layer++) for (unsigned y=0; y<size; y++) for (unsigned x=0; x<size; x++) {
            uint32_t expected=0x11223344;
            int drawn=layer>=first && x>=1 && y>=1 && x<size-1 && y<size-1 && (!depth_test || layer%2);
            if (drawn) expected=100+layer-first;
            if (drawn && layer==first && x==1 && y==1) expected=109;
            unsigned i=(layer*size+y)*size+x;
            if (returned[i]!=expected) die("layered shape%u mip%u first%u layer%u (%u,%u): %#x expected %#x",
                shape,mip,first,layer,x,y,returned[i],expected);
            if (second[i]!=(drawn ? expected+200 : expected) || returned_depths[i]!=(drawn && depth_test ? .25f : depths[i]))
                die("layered MRT/depth shape%u mip%u first%u test%u layer%u (%u,%u)",shape,mip,first,depth_test,layer,x,y);
            checks+=3;
        }
        mtl_tex_release(&target);mtl_tex_release(&other);mtl_tex_release(&depth);
    }
    mtl_queue_forget(slot);
    printf("Layered array, cube and 3D attachments passed %u mip, slice, scissor, MRT, depth and ordinary-draw checks\n",checks);
}

static void layered_clears(void) {
    enum { slot=1596 };
    mtl_queue_activate(slot);
    for (int volume=0; volume<2; volume++) for (int partial=0; partial<2; partial++) {
        tex_t color={.serial=0x17940,.d={.width=8,.height=8,.depth=8,.levels=2,
            .format=NVN_FORMAT_RGBA8,.target=volume ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D_ARRAY}};
        tex_t depth=color,small=color;depth.serial++;small.serial+=2;
        depth.d.target=NVN_TEXTURE_TARGET_2D_ARRAY;depth.d.format=NVN_FORMAT_DEPTH32F;
        small.d=(nvn_texdesc){.width=2,.height=2,.depth=1,.levels=1,.format=NVN_FORMAT_RGBA8,.target=NVN_TEXTURE_TARGET_2D};
        nvn_tex_layout(&color.d,&color.L);nvn_tex_layout(&depth.d,&depth.L);nvn_tex_layout(&small.d,&small.L);
        uint8_t initial[8*4*4*4],pixels[sizeof initial];float initial_depth[8*4*4],depth_pixels[8*4*4];
        memset(initial,64,sizeof initial);for (unsigned i=0;i<sizeof initial_depth/sizeof *initial_depth;i++) initial_depth[i]=.5f;
        view_t level={.set_levels=1,.base_level=1,.nlevels=1};
        view_t view=level;view.set_layers=1;view.min_layer=1;view.nlayers=2;
        unsigned layers=volume ? 4 : 8;
        list_t list={0};
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&color,.has_view=1,.view=level,.src=(uintptr_t)initial,.r={.width=4,.height=4,.depth=layers});
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&depth,.has_view=1,.view=level,.src=(uintptr_t)initial_depth,.r={.width=4,.height=4,.depth=8});
        RECORD(OP_SET_RT,p_set_rt,.n=2,.color={&small,&color},.has_cview={0,1},.cview={{0},view},.depth=&depth,.has_dview=1,.dview=view);
        RECORD(OP_SCISSOR,p_scissor,.x=partial,.y=partial,.w=4-partial,.h=4-partial);
        RECORD(OP_CLEAR_COLOR,p_clear_color,.index=1,.mask=partial ? 5 : 15,.rgba={1,0,1,0});
        RECORD(OP_CLEAR_DS,p_clear_ds,.depth=.75f,.depth_mask=1);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.has_view=1,.view=level,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=layers});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&depth,.has_view=1,.view=level,.src=(uintptr_t)depth_pixels,.r={.width=4,.height=4,.depth=8});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (unsigned z=0;z<8;z++) for (unsigned y=0;y<4;y++) for (unsigned x=0;x<4;x++) {
            unsigned i=(z*4+y)*4+x;int changed=z>=1 && z<3 && x>=(unsigned)partial && y>=(unsigned)partial;
            if (depth_pixels[i]!=(changed ? .75f : .5f)) die("layered clear depth volume%d partial%d (%u,%u,%u)",volume,partial,x,y,z);
            if (z<layers) for (unsigned c=0;c<4;c++) {
                unsigned expected=changed && (!partial || !(c&1)) ? (c&1 ? 0 : 255) : 64;
                if (pixels[4*i+c]!=expected) die("layered clear color volume%d partial%d (%u,%u,%u) channel%u: %u expected %u",volume,partial,x,y,z,c,pixels[4*i+c],expected);
            }
        }
        mtl_tex_release(&color);mtl_tex_release(&depth);mtl_tex_release(&small);
    }
    tex_t packed={.serial=0x17950,.d={.width=4,.height=2,.depth=4,.levels=1,
        .format=NVN_FORMAT_DEPTH32F_STENCIL8,.target=NVN_TEXTURE_TARGET_2D_ARRAY}};
    nvn_tex_layout(&packed.d,&packed.L);
    struct { float depth; uint32_t stencil; } initial[32],result[32];
    for (unsigned i=0;i<32;i++) { initial[i].depth=.5f;initial[i].stencil=0xa5; }
    view_t view={.set_layers=1,.min_layer=1,.nlayers=2};
    list_t list={0};
    RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&packed,.src=(uintptr_t)initial,.r={.width=4,.height=2,.depth=4});
    RECORD(OP_SET_RT,p_set_rt,.depth=&packed,.has_dview=1,.dview=view);
    RECORD(OP_SCISSOR,p_scissor,.x=1,.y=1,.w=2,.h=1);
    RECORD(OP_CLEAR_DS,p_clear_ds,.depth=.75f,.depth_mask=1,.stencil=0x13,.stencil_mask=15);
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&packed,.src=(uintptr_t)result,.r={.width=4,.height=2,.depth=4});
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (unsigned i=0;i<32;i++) {
        unsigned layer=i/8,y=i/4%2,x=i%4;
        int changed=layer>=1 && layer<3 && y==1 && x>=1 && x<3;
        if (result[i].depth!=(changed ? .75f : .5f) || (result[i].stencil&255)!=(changed ? 0xa3u : 0xa5u))
            die("layered masked stencil at word%u",i);
    }
    mtl_tex_release(&packed);mtl_queue_forget(slot);
    puts("Layered clears preserve mip/layer guards, component/stencil masks and attachment extents beside a smaller target");
}
