// The component-wise footprint oracle also backs the public DrawTexture test.
// This path exercises an ordinary guest shader and separate texture/sampler slots.
static void sampler_reduction(void) {
    enum { slot = 1512 };
    typedef struct { float u, v, lod; int filter, mag; float expected[3]; int border; float bias; } sample_case;
    static const sample_case cases[] = {
        {.25f,.25f,1, NVN_MIN_FILTER_LINEAR,1, {4,1,7}},
        {.5f,.5f,1, NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST,1, {12,9,15}},
        {.375f,.375f,0, NVN_MIN_FILTER_LINEAR,1, {7,7,7}},
        {.5f,.5f,.5f, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,1, {8,1,15}},
        {0,.125f,0, NVN_MIN_FILTER_LINEAR,1, {2,1,3}},
        {.5f,.5f,1, NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST,0, {15,15,15}},
        {.5f,.5f,.25f, NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST,1, {4,1,15}},
        {.5f,.5f,.25f, NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST,0, {1,1,15}},
        {0,.125f,0, NVN_MIN_FILTER_LINEAR,1, {.5f,0,1},1},
        {-1,.125f,0, NVN_MIN_FILTER_LINEAR,1, {0,0,0},1},
        {.25f,.25f,-1, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,0, {7,7,7}},
        {.25f,.25f,1, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,0, {9,9,9}},
        {.5f,.5f,.5f, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,1, {12,9,15},0,.5f},
    };
    static const sample_case offset_cases[] = {
        {.125f,.125f,0, NVN_MIN_FILTER_LINEAR,1, {7,7,7}},
        {.25f,.25f,0, NVN_MIN_FILTER_LINEAR,1, {4,1,7}},
        {.25f,.25f,1, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,1, {15,15,15}},
        {-.25f,.125f,0, NVN_MIN_FILTER_LINEAR,1, {2.5f,0,5},1},
        {-1,.125f,0, NVN_MIN_FILTER_LINEAR,1, {0,0,0},1},
        {.5f,.5f,.5f, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,1, {8,1,15}},
    };
    // The volume's third coordinate equals u. Alternating z slices add eight
    // to the first mip, and the second mip contains every odd value 17..31.
    static const sample_case volume_cases[] = {
        {.25f,.25f,1, NVN_MIN_FILTER_LINEAR,1, {8,1,15}},
        {.5f,.5f,1, NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST,1, {24,17,31}},
        {.375f,.375f,0, NVN_MIN_FILTER_LINEAR,1, {15,15,15}},
        {.5f,.5f,.5f, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,1, {16,1,31}},
        {0,.125f,0, NVN_MIN_FILTER_LINEAR,1, {6,1,11}},
        {.5f,.5f,1, NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST,0, {31,31,31}},
        {.5f,.5f,.25f, NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST,1, {8,1,31}},
        {.5f,.5f,.25f, NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST,0, {1,1,31}},
        {0,.125f,0, NVN_MIN_FILTER_LINEAR,1, {2.5f,0,9},1},
        {-1,.125f,0, NVN_MIN_FILTER_LINEAR,1, {0,0,0},1},
        {.25f,.25f,-1, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,0, {15,15,15}},
        {.25f,.25f,1, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,0, {17,17,17}},
        {.5f,.5f,.5f, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,1, {24,17,31},0,.5f},
    };
    static const sample_case volume_offset_cases[] = {
        {.125f,.125f,0, NVN_MIN_FILTER_LINEAR,1, {15,15,15}},
        {.25f,.25f,0, NVN_MIN_FILTER_LINEAR,1, {8,1,15}},
        {.25f,.25f,1, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,1, {31,31,31}},
        {-.25f,.125f,0, NVN_MIN_FILTER_LINEAR,1, {4.5f,0,13},1},
        {-1,.125f,0, NVN_MIN_FILTER_LINEAR,1, {0,0,0},1},
        {.5f,.5f,.5f, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,1, {16,1,31}},
    };
    const NVNformat formats[] = {NVN_FORMAT_R32F,NVN_FORMAT_RGBA32F,NVN_FORMAT_RGBA8,NVN_FORMAT_DEPTH32F};
    mtl_queue_activate(slot);
    tex_t target = {.serial=0x17600, .d={.width=2,.height=2,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA32F}};
    nvn_tex_layout(&target.d,&target.L);
    unsigned checks = 0;
    for (int shape = 0; shape < 3; shape++) for (int format = 0; format < (shape == 2 ? 3 : 4); format++) {
        int array = shape == 1, volume = shape == 2, base_texels = volume ? 64 : 16;
        tex_t texture = {.serial=0x17500+4*shape+format, .d={.width=4,.height=4,.depth=volume ? 4 : array ? 2 : 1,.levels=2,
            .target=volume ? NVN_TEXTURE_TARGET_3D : array ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D,.format=formats[format]}};
        nvn_tex_layout(&texture.d,&texture.L); mtl_reg_tex(slot,&texture,NULL);
        float rgba[2][72][4]; uint8_t bytes[2][72][4]; float red[2][72];
        for (int layer = 0; layer <= array; layer++) for (int i = 0; i < (volume ? 72 : 20); i++) {
            float value = i < base_texels ? 1+2*(i%2)+4*(i/4%2)+(volume ? 8*(i/16%2) : 0) :
                (volume ? 17 : 9)+2*(i-base_texels);
            if (array && !layer) value += 100;
            red[layer][i] = format == 3 ? value/128 : value;
            memcpy(rgba[layer][i], (float[]){value,120-value,2*value,1}, sizeof rgba[layer][i]);
            memcpy(bytes[layer][i], (uint8_t[]){value,120-value,2*value,255}, sizeof bytes[layer][i]);
        }
        list_t upload={0};
        for (int layer = 0; layer <= array; layer++) for (int mip = 0; mip < 2; mip++) {
            int offset = mip ? base_texels : 0;
            uintptr_t source = format == 0 || format == 3 ? (uintptr_t)(red[layer]+offset) :
                format == 1 ? (uintptr_t)(rgba[layer]+offset) : (uintptr_t)(bytes[layer]+offset);
            p_copy_b2t copy={.tex=&texture,.src=source,.has_view=1,.view={.set_levels=1,.base_level=mip},
                .r={.width=4>>mip,.height=4>>mip,.zoffset=layer,.depth=volume ? 4>>mip : 1}};
            put(&upload,OP_COPY_B2T,&copy,sizeof copy);
        }
        put(&upload,OP_END,NULL,0); mtl_sub_begin();mtl_exec(upload.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (int form=0;form<13;form++) for (int separate=0;separate<2;separate++) {
          if (form >= 8 && form < 12 && array) continue; // SPIR-V projected sampling has no array form.
          int offset = form < 8 && (form & 2);
          const sample_case *table = volume ? (offset ? volume_offset_cases : volume_cases) : (offset ? offset_cases : cases);
          unsigned count = offset ? sizeof offset_cases/sizeof *offset_cases : sizeof cases/sizeof *cases;
          int fragment = (form >= 4 && form < 8) || form >= 10, stage = fragment ? 1 : 5;
          prog_t *program = &sampler_reduction_program[form][shape][separate]; program->nstage=fragment ? 2 : 1;
          for (unsigned test=0;test<count;test++) for (int mode=0;mode<3;mode++) {
            const sample_case *c=table+test;
            // Native gradient LOD may be approximated. Exact powers of two
            // check averaging, while fractional footprints check both extrema.
            if ((fragment || (form & 1)) && mode == 0 && c->lod != floorf(c->lod)) continue;
            float green_border = format == 1 ? 120 : format == 2 ? 120.0f/255 : 0;
            samp_par sampler={.ws=c->border ? NVN_WRAP_MODE_CLAMP_TO_BORDER : NVN_WRAP_MODE_REPEAT,
                .wt=NVN_WRAP_MODE_REPEAT,.wr=NVN_WRAP_MODE_REPEAT,.border={0,green_border,0,1},
                .minf=c->filter,.magf=c->mag,.lod_bias=c->bias,.lod_max=10,.reduction=mode};
            mtl_reg_samp(slot,&sampler);
            float coordinate[]={c->u,c->v,c->lod,volume ? c->u : array}, result[16]; memset(result,0xcc,sizeof result);
            list_t list={0};
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=program,.stages=fragment ? 3 : 0x20);
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=stage|BIND_TEX1_NOBASE,.slot=0,.h=((uint64_t)slot<<20)|slot);
            RECORD(OP_BIND_SAMP,p_bind_handles,.stage=stage,.first=1,.n=1,.h={(uint64_t)slot<<20});
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=stage,.index=0,.addr=(uintptr_t)coordinate,.size=sizeof coordinate);
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
            put(&list,OP_END,NULL,0); mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            float expected[]={c->expected[mode],0,0,1};
            if (form == 12) {
                expected[1] = c->lod+c->bias;
                expected[0] = c->filter >= 2 ? fminf(1,fmaxf(0,expected[1])) : 0;
                if (!mode && c->filter < 4) expected[0] = floorf(expected[0]+.5f);
                expected[2] = 0;
            } else {
                if (format == 1 || format == 2) { expected[1]=120-c->expected[mode ? 3-mode : 0]; expected[2]=2*expected[0]; }
                if (format==2) for (int ch=0;ch<3;ch++) expected[ch]/=255;
                if (format==3) expected[0]/=128;
            }
            for (int ch=0;ch<4;ch++) if (!isfinite(result[ch]) || fabsf(result[ch]-expected[ch])>0.00001f)
                die("sampler reduction form%d shape%d format%d separate%d case%u mode%d channel%d: %g expected %g",form,shape,format,separate,test,mode,ch,result[ch],expected[ch]);
            checks++;
          }
        }
        mtl_reg_tex(slot,NULL,NULL);mtl_tex_release(&texture);
    }
    mtl_tex_release(&target); mtl_queue_forget(slot);
    printf("Guest sampler average/min/max passed %u format, independent binding, mip and spatial footprint cases\n",checks);
}
