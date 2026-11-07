static void sampler_gather(void) {
    enum { slot=1680 };
    unsigned checks=0;mtl_queue_activate(slot);
    const NVNtextureTarget targets[]={NVN_TEXTURE_TARGET_2D,NVN_TEXTURE_TARGET_2D_ARRAY,
        NVN_TEXTURE_TARGET_CUBEMAP,NVN_TEXTURE_TARGET_CUBEMAP_ARRAY};
    // ConstOffsets selects the lower-left texel of each separately offset
    // footprint, unlike the four different corners of a single-offset gather.
    const float gathered[][4]={{10,11,7,6},{7,8,4,3},{5,7,2,10}};
    for (int shape=0;shape<4;shape++) for (int shadow=0;shadow<2;shadow++) {
        int cube=shape>=2,array=shape&1,layers=(array+1)*(cube ? 6 : 1);
        tex_t source={.serial=0x18e00+shape*2+shadow,.d={.width=4,.height=4,.depth=layers,.levels=2,
            .target=targets[shape],.format=shadow ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA32F}};
        nvn_tex_layout(&source.d,&source.L);mtl_reg_tex(slot,&source,NULL);
        float depth[16],color[16][4];
        for (int mip=0;mip<2;mip++) for (int layer=0;layer<layers;layer++) {
            int base=cube ? 64*(layer/6)+16*(layer%6) : 64*layer,size=4>>mip;
            for (int y=0;y<size;y++) for (int x=0;x<size;x++) {
                int i=y*size+x;float value=base+100*mip+x+4*y+1;
                depth[i]=value/256;memcpy(color[i],(float[]){value,200-value,2*value,1},16);
            }
            list_t list={0};
            RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&source,.has_view=1,.view={.set_levels=1,.base_level=mip},
                .src=shadow ? (uintptr_t)depth : (uintptr_t)color,.r={.width=size,.height=size,.zoffset=layer,.depth=1});
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        }
        float base=64*array+(cube ? 64 : 0);
        for (int form=0;form<(cube ? 1 : 3);form++) for (int separate=0;separate<2;separate++)
        for (int linear=0;linear<2;linear++) for (int mode=0;mode<3;mode++) {
            // Gather always reads the view's base mip. These nonzero limits,
            // bias and anisotropy must not select a mip or filter the results.
            // llvmpipe's native anisotropic gather averages texels (8.5 instead
            // of 10 in the first case). Keep the average-mode control isotropic;
            // the min/max variants still test the guest's anisotropic sampler.
            samp_par sampler={.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE,
                .minf=linear ? 5 : 2,.magf=linear,.lod_min=1,.lod_max=1,.lod_bias=1,.aniso=mode ? 4 : 1,.reduction=mode,
                .cmp_mode=shadow,.cmp_func=NVN_COMPARE_FUNC_LESS};
            mtl_reg_samp(slot,&sampler);
            float parameters[2][4]={{.5f,.5f,array,0},{(base+8.5f)/256}},result[4]={-1,-1,-1,-1};
            if (cube) memcpy(parameters[0],(float[]){0,0,2,array},16);
            prog_t *program=&sampler_gather_program[(shape*3+form)*4+shadow*2+separate];program->nstage=1;
            list_t list={0};
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=program,.stages=0x20);
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=0,.h=((uint64_t)slot<<20)|slot);
            RECORD(OP_BIND_SAMP,p_bind_handles,.stage=5,.first=0,.n=1,.h={(uint64_t)slot<<20});
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)parameters,.size=sizeof parameters);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)result,.size=sizeof result);
            RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (int ch=0;ch<4;ch++) {
                float expected=shadow ? gathered[form][ch]>8.5f : base+gathered[form][ch];
                if (result[ch]!=expected) die("gather shape%d shadow%d form%d separate%d linear%d mode%d channel%d: %g expected %g",
                    shape,shadow,form,separate,linear,mode,ch,result[ch],expected);
            }
            checks++;
        }
        mtl_reg_tex(slot,NULL,NULL);mtl_tex_release(&source);
    }
    mtl_queue_forget(slot);
    printf("Reduction samplers preserve all four gather texels in %u color/depth, offset, cube/array and independent binding cases\n",checks);
}
