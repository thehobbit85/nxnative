// Sample the same depth image through two registrations. Both keep selecting
// the same mip and array layer as that attachment is cleared again.
static void depth_image_views(void) {
    mtl_queue_activate(1430);
    const NVNformat formats[]={NVN_FORMAT_DEPTH16,NVN_FORMAT_DEPTH32F,NVN_FORMAT_DEPTH24_STENCIL8,NVN_FORMAT_DEPTH32F_STENCIL8};
    samp_par nearest={.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE};
    mtl_reg_samp(1417,&nearest);
    for (int i=0;i<4;i++) for (int builder_stencil=0;builder_stencil<(i<2 ? 1 : 2);builder_stencil++) {
        tex_t t={.serial=0x17130+2*i+builder_stencil,.stencil=builder_stencil,
                 .d={.width=4,.height=4,.depth=2,.levels=2,.samples=1,.target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=formats[i]}};
        nvn_tex_layout(&t.d,&t.L);
        view_t sub={.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=1,.nlayers=1};
        view_t depth=sub,stencil=sub;
        depth.set_dsmode=builder_stencil; depth.dsmode=NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
        if (i<2) { depth.set_format=1; depth.format=i==0 ? NVN_FORMAT_R16 : NVN_FORMAT_R32F; }
        else { depth.set_swizzle=1; memcpy(depth.swz,(uint8_t[]){2,0,0,1},4); }
        stencil.set_dsmode=!builder_stencil; stencil.dsmode=NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
        mtl_reg_tex(1430,&t,&depth);
        if (i>=2) mtl_reg_tex(1431,&t,&stencil);
        for (int pass=0;pass<2;pass++) {
            float z=pass ? .75f : .25f;
            unsigned st=pass ? 0xa5 : 0x46;
            list_t list={0};
            RECORD(OP_SET_RT,p_set_rt,.depth=&t,.has_dview=1,.dview=sub);
            RECORD(OP_SCISSOR,p_scissor,.w=2,.h=2);
            RECORD(OP_CLEAR_DS,p_clear_ds,.depth=z,.depth_mask=1,.stencil=st,.stencil_mask=i<2 ? 0 : 255);
            RECORD(OP_SET_RT,p_set_rt,.n=0);
            struct { int32_t s[4]; uint32_t u[4]; float f[4]; } result={0};
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&typed_border_program[0],.stages=0x20);
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=0,.h=0);
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=1,.h=i<2 ? 0 : ((1417ull<<20)|1431));
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=2,.h=(1417ull<<20)|1430);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)&result,.size=sizeof result);
            RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
            put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            if (!(fabsf(result.f[0]-z)<0.00002f) || result.f[1] || result.f[2] || result.f[3]!=1)
                die("depth view %#x builder stencil%d: (%g,%g,%g,%g) expected (%g,0,0,1)",formats[i],
                    builder_stencil,result.f[0],result.f[1],result.f[2],result.f[3],z);
            if (i>=2 && memcmp(result.u,(uint32_t[]){st,0,0,1},sizeof result.u))
                die("stencil view %#x builder stencil%d: (%u,%u,%u,%u) expected (%u,0,0,1)",formats[i],
                    builder_stencil,result.u[0],result.u[1],result.u[2],result.u[3],st);
        }
        mtl_reg_tex(1430,NULL,NULL); if (i>=2) mtl_reg_tex(1431,NULL,NULL); mtl_tex_release(&t);
    }
    mtl_queue_forget(1430);
    puts("Depth-as-red and stencil-as-uint views follow render writes and guest aspect overrides");
}
