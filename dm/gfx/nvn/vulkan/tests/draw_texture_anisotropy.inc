// Every mip has a separate value range. These extrema distinguish anisotropic
// footprints from the isotropic (major-axis) mip and from weighted averaging.
static void draw_texture_anisotropy(void) {
    enum { slot=1660 };
    tex_t source={.serial=0x18c00,.d={.width=16,.height=16,.depth=1,.levels=5,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA32F}};
    tex_t target={.serial=0x18c01,.d={.width=1,.height=1,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA32F}};
    nvn_tex_layout(&source.d,&source.L);nvn_tex_layout(&target.d,&target.L);
    mtl_queue_activate(slot);mtl_reg_tex(slot,&source,NULL);
    float pixels[5][256][4];list_t upload={0};
    for (int mip=0;mip<5;mip++) {
        int size=16>>mip;
        for (int y=0;y<size;y++) for (int x=0;x<size;x++) {
            float value=1000*mip+3*x+5*y;
            memcpy(pixels[mip][y*size+x],(float[]){value,20000-value,2*value,1},sizeof pixels[0][0]);
        }
        p_copy_b2t copy={.tex=&source,.has_view=1,.view={.set_levels=1,.base_level=mip},
            .src=(uintptr_t)pixels[mip],.r={.width=size,.height=size,.depth=1}};
        put(&upload,OP_COPY_B2T,&copy,sizeof copy);
    }
    put(&upload,OP_END,NULL,0);mtl_sub_begin();mtl_exec(upload.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    const struct { float dx,dy,aniso,lo,hi,bias; int filter; float minimum,maximum; } cases[]={
        {4,1,4,0,4,0,5,53,67}, {8,1,4,0,4,0,5,1021,1035}, {8,2,4,0,4,0,5,1021,1035},
        {1,4,4,0,4,0,5,51,69}, {1,8,4,0,4,0,5,1019,1037}, {-4,1,4,0,4,0,5,53,67},
        {4,4,4,0,4,0,5,2008,2016}, {4,1,1,0,4,0,5,2008,2016},
        {4,1,3.9f,0,4,0,5,53,1035}, {4,1,4,1,1,0,5,1021,1035},
        {4,1,4,0,4,1,5,1021,1035}, {8,1,4,0,4,0,1,47,73},
        {1,4,4,0,4,0,4,54,69}, {16,1,16,0,4,0,5,35,85},
        {4,1,3.9f,0,4,0,3,53,1035},
    };
    for (unsigned test=0;test<sizeof cases/sizeof cases[0];test++) for (int mode=1;mode<=2;mode++) {
        const typeof(cases[0]) *c=cases+test;
        samp_par sampler={.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,
            .minf=c->filter,.magf=c->filter&1,.lod_min=c->lo,.lod_max=c->hi,.lod_bias=c->bias,.aniso=c->aniso,.reduction=mode};
        mtl_reg_samp(slot,&sampler);
        float result[4]={-1,-1,-1,-1};list_t list={0};
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
        RECORD(OP_SCISSOR,p_scissor,.w=1,.h=1);
        RECORD(OP_DRAW_TEX,p_draw_tex,.h=((uint64_t)slot<<20)|slot,.d={0,0,1,1},
            .s={8-c->dx/2,8-c->dy/2,8+c->dx/2,8+c->dy/2});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)result,.r={.width=1,.height=1,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        float expected[]={mode==1 ? c->minimum : c->maximum,20000-(mode==1 ? c->maximum : c->minimum),
            2*(mode==1 ? c->minimum : c->maximum),1};
        for (int ch=0;ch<4;ch++) if (!isfinite(result[ch]) || fabsf(result[ch]-expected[ch])>.001f)
            die("DrawTexture anisotropy case%u mode%d channel%d: %g expected %g",test,mode,ch,result[ch],expected[ch]);
    }
    mtl_reg_tex(slot,NULL,NULL);mtl_tex_release(&source);mtl_tex_release(&target);mtl_queue_forget(slot);
    puts("DrawTexture anisotropic min/max passed 30 directional, ratio, mip, bias, clamp and nearest cases");
}
