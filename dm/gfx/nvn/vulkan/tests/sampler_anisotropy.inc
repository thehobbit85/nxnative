static void sampler_anisotropy(void) {
    enum { slot=1670 };
    tex_t target={.serial=0x18d10,.d={.width=2,.height=2,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA32F}};
    nvn_tex_layout(&target.d,&target.L);mtl_queue_activate(slot);
    const NVNtextureTarget targets[]={NVN_TEXTURE_TARGET_2D,NVN_TEXTURE_TARGET_2D_ARRAY,
        NVN_TEXTURE_TARGET_3D,NVN_TEXTURE_TARGET_CUBEMAP,NVN_TEXTURE_TARGET_CUBEMAP_ARRAY};
    // Analytic extrema for an eight-texel horizontal and one-texel vertical
    // footprint. Every mip has a disjoint range; volume Z contributes 7*z.
    const struct { int anisotropy; float minimum,maximum,volume_minimum,volume_maximum,major; } cases[]={
        {1,3000,3008,3000,3015}, {2,2008,2016,2015,2030}, {3,1021,2019,1042,2033},
        {4,1021,1035,1042,1063}, {8,47,73,96,129}, {16,35,85,84,141,16},
    };
    unsigned checks=0;
    for (int shape=0;shape<5;shape++) {
        int array=shape==1 || shape==4,cube=shape>=3,volume=shape==2;
        tex_t source={.serial=0x18d00+shape,.d={.width=16,.height=16,.depth=volume ? 16 : (array+1)*(cube ? 6 : 1),
            .levels=5,.target=targets[shape],.format=NVN_FORMAT_RGBA32F}};
        nvn_tex_layout(&source.d,&source.L);mtl_reg_tex(slot,&source,NULL);
        for (int mip=0;mip<5;mip++) for (int layer=0;layer<=(array ? 1 : 0);layer++) for (int face=0;face<(cube ? 6 : 1);face++) {
            int size=16>>mip,depth=volume ? size : 1;
            float *pixels=malloc((size_t)size*size*depth*16);assert(pixels);
            for (int z=0;z<depth;z++) for (int y=0;y<size;y++) for (int x=0;x<size;x++) {
                float value=20000*layer+5000*face+1000*mip+3*x+5*y+7*z;
                memcpy(pixels+4*((z*size+y)*size+x),(float[]){value,100000-value,2*value,1},16);
            }
            list_t list={0};
            RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&source,.has_view=1,.view={.set_levels=1,.base_level=mip},
                .src=(uintptr_t)pixels,.r={.zoffset=cube ? layer*6+face : layer,.width=size,.height=size,.depth=depth});
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));free(pixels);
        }
        for (int form=0;form<4;form++) for (int separate=0;separate<2;separate++) {
            int fragment=form>=2,stage=fragment ? 1 : 5;
            prog_t *program=&sampler_anisotropy_program[shape*8+form*2+separate];program->nstage=fragment ? 2 : 1;
            for (unsigned test=0;test<sizeof cases/sizeof cases[0];test++) for (int mode=1;mode<=2;mode++) {
                if ((shape || form!=1 || separate) && test!=3) continue;
                const typeof(cases[0]) *c=cases+test;
                samp_par sampler={.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE,
                    .minf=NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,.magf=1,.lod_max=4,.aniso=c->anisotropy,.reduction=mode};
                mtl_reg_samp(slot,&sampler);
                float parameters[4][4]={{.5f,.5f,volume ? .5f : array,0},{.5f,0,0,0},{0,1.0f/16,0,0},{1.5f}},result[16];
                if (c->major) parameters[1][0]=c->major/16;
                if (cube) {
                    memcpy(parameters[0],(float[]){0,0,2,array},16);
                    memcpy(parameters[1],(float[]){2,0,0,0},16);
                    memcpy(parameters[2],(float[]){0,-.25f,0,0},16);
                }
                memset(result,0xcc,sizeof result);list_t list={0};
                RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=program,.stages=fragment ? 3 : 0x20);
                RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=stage|BIND_TEX1_NOBASE,.slot=0,.h=((uint64_t)slot<<20)|slot);
                RECORD(OP_BIND_SAMP,p_bind_handles,.stage=stage,.first=0,.n=1,.h={(uint64_t)slot<<20});
                RECORD(OP_BIND_UBO,p_bind_buf,.stage=stage,.index=0,.addr=(uintptr_t)parameters,.size=sizeof parameters);
                if (fragment) {
                    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
                    RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,2,2}});
                    RECORD(OP_SCISSOR,p_scissor,.w=2,.h=2);
                    RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={-100,-100,-100,-100});
                    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
                    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)result,.r={.width=2,.height=2,.depth=1});
                } else {
                    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)result,.size=sizeof result);
                    RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
                }
                put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
                float base=20000*array+20000*cube,lo=volume ? c->volume_minimum : c->minimum,hi=volume ? c->volume_maximum : c->maximum;
                if (!form) { lo=volume ? 1045 : 1024;hi=volume ? 2030 : 2016; }
                lo+=base;hi+=base;
                float expected[]={mode==1 ? lo : hi,100000-(mode==1 ? hi : lo),2*(mode==1 ? lo : hi),1};
                if (form==3) memcpy(expected,(float[]){1,1,0,1},sizeof expected);
                for (int ch=0;ch<4;ch++) if (!isfinite(result[ch]) || fabsf(result[ch]-expected[ch])>.001f)
                    die("guest anisotropy shape%d form%d separate%d test%u mode%d ch%d: %g expected %g",shape,form,separate,test,mode,ch,result[ch],expected[ch]);
                checks++;
            }
        }
        if (!shape) for (int separate=0;separate<2;separate++) for (int rotation=0;rotation<3;rotation++) {
            for (int j=0;j<3;j++) {
                int mode=(j+rotation)%3;
                mtl_reg_tex(slot+j,&source,NULL);
                mtl_reg_samp(slot+j,&(samp_par){.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,
                    .minf=5,.magf=1,.lod_max=4,.reduction=mode,.aniso=mode==0 ? 1 : mode==1 ? 2 : 8});
            }
            for (int index=0;index<3;index++) {
                prog_t *program=&sampler_anisotropy_program[40+separate];program->nstage=1;
                float parameters[4][4]={{.5f,.5f,0,0},{.5f,0,0,0},{0,1.0f/16,0,0},{0,index}},result[4]={0};
                list_t list={0};
                RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=program,.stages=0x20);
                for (int j=0;j<3;j++) {
                    RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=j,.h=((uint64_t)(slot+j)<<20)|(slot+j));
                    RECORD(OP_BIND_SAMP,p_bind_handles,.stage=5,.first=j,.n=1,.h={(uint64_t)(slot+j)<<20});
                }
                RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)parameters,.size=sizeof parameters);
                RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)result,.size=sizeof result);
                RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
                put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
                int mode=(index+separate+rotation)%3;
                float red=mode==0 ? 3004 : mode==1 ? 2008 : 73,other=mode==0 ? 3004 : mode==1 ? 2016 : 47;
                float expected[]={red,100000-other,2*red,1};
                for (int ch=0;ch<4;ch++) if (!isfinite(result[ch]) || fabsf(result[ch]-expected[ch])>.001f)
                    die("anisotropic array separate%d rotation%d index%d channel%d: %g expected %g",separate,rotation,index,ch,result[ch],expected[ch]);
                checks++;
            }
        }
        for (int j=0;j<3;j++) mtl_reg_tex(slot+j,NULL,NULL);
        mtl_tex_release(&source);
    }
    mtl_tex_release(&target);mtl_queue_forget(slot);
    printf("Guest anisotropic min/max passed %u shape, binding, gradient, implicit/query and explicit-LOD cases\n",checks);
}
