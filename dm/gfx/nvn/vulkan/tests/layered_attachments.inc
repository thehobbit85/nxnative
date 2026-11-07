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
