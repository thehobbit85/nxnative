// One moving edge with a textured foreground, encoded through the public replay
// record. Compare the same final frame with history, zero motion, and a reset.
static tex_t dlss_surface(uint64_t serial, unsigned w, unsigned h, int format) {
    tex_t t={.magic=MAGIC('T','E','X',0),.serial=serial,.off=serial<<24,
        .d={.target=NVN_TEXTURE_TARGET_2D,.format=format,.width=w,.height=h,
            .depth=1,.samples=1,.levels=1,.flags=NVN_TEXTURE_FLAGS_IMAGE}};
    nvn_tex_layout(&t.d,&t.L);
    return t;
}
static void dlss_upload(list_t *list, tex_t *t, void *data) {
    p_copy_b2t copy={.tex=t,.src=(uintptr_t)data,.r={.width=t->d.width,.height=t->d.height,.depth=1}};
    put(list,OP_COPY_B2T,&copy,sizeof copy);
}
static void dlss_temporal_sequence(int auto_exposure) {
    enum { W=128,H=64, OW=2*W,OH=2*H, N=W*H, NP=OW*OH, MODES=3 };
    tex_t col=dlss_surface(0x8900,W,H,NVN_FORMAT_RGBA16F), dep=dlss_surface(0x8901,W,H,NVN_FORMAT_DEPTH32F),
        vel=dlss_surface(0x8902,W,H,NVN_FORMAT_RG16F), out=dlss_surface(0x8903,OW,OH,NVN_FORMAT_RGBA16F),
        exp=dlss_surface(0x8904,1,1,NVN_FORMAT_R16F);
    size_t len=16*N+8*NP*MODES+16384;
    __fp16 *mem; assert(!posix_memalign((void **)&mem,getpagesize(),len)); memset(mem,0,len);
    __fp16 *color=mem, *motion=color+4*N, *exposure=motion+2*N;
    float *depth=(float *)(exposure+8);
    __fp16 *pixels=(__fp16 *)(depth+N);
    mtl_pool_add(mem,len); mtl_queue_activate(col.serial);
    *exposure=auto_exposure ? .125f : 2.0f;
    for (int frame=0;frame<5;frame++) {
        int edge=38+3*frame;
        float pre=frame<3 ? .75f : 1.5f;
        for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
            int i=y*W+x, fg=x>=edge && x<edge+48;
            float red=fg ? .75f : .125f;
            color[4*i]=pre*red; color[4*i+1]=pre*(fg ? .38f+.23f*((x-edge+y+frame)%7<3) : .125f);
            color[4*i+2]=pre*red; color[4*i+3]=fg?.5f:1;
            depth[i]=auto_exposure ? (fg?.2f:.75f) : (fg?.8f:.25f);
            motion[2*i]=fg?-1.5f:0; motion[2*i+1]=0;
        }
        list_t list={0};
        dlss_upload(&list,&col,color); dlss_upload(&list,&dep,depth); dlss_upload(&list,&vel,motion);
        dlss_upload(&list,&exp,exposure);
        for (int mode=0;mode<MODES;mode++) {
            RECORD(OP_DLSS,p_dlss,.tex={&col,&dep,&vel,&out,NULL,&exp},.net=0x8900+mode,
                .iw=W,.ih=H,.ow=OW,.oh=OH,.flags=auto_exposure ? DLSS_F_AUTO_EXPOSURE : DLSS_F_DEPTH_INVERTED,.mv={mode==1?0:2,1},
                .jitter={frame%2?.25f:-.25f,frame%2?-.25f:.25f},.pre_exposure=pre,.reset=!frame || mode==2);
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&out,.src=(uintptr_t)(pixels+4*NP*mode),
                .r={.width=OW,.height=OH,.depth=1});
        }
        put(&list,OP_END,NULL,0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    }
    float history=0, motion_diff=0, green=0;
    for (int y=OH/4;y<3*OH/4;y++) {
        int first=OW, last=-1;
        for (int x=0;x<OW;x++) {
            int i=4*(y*OW+x);
            float red=pixels[i];
            assert(pixels[i+3]==1);   // MetalFX outputs opaque even for alpha 0.5
            if (red>.65f) { if (x<first) first=x; last=x; }
            // The plateau retains the original pre-exposed HDR units even
            // when the exposure texture or internal meter changes the filter.
            if (x<99 || x>198) assert(fabsf(red-.1875f)<.035f);
            if (x>103 && x<192) assert(fabsf(red-1.125f)<.035f);
            if (x<110 || x>=185) continue;
            green+=pixels[i+1];
            history+=fabsf((float)pixels[i+1]-pixels[8*NP+i+1]);
            motion_diff+=fabsf((float)pixels[i+1]-pixels[4*NP+i+1]);
        }
        // At 2x the final foreground occupies output columns [100,196).
        assert(abs(first-100)<=2 && abs(last-195)<=2);
    }
    float samples=(OH/2)*75;
    fprintf(stderr,"DLSS temporal %s exposure: mean green %.5f, history delta %.5f, motion delta %.5f\n",
        auto_exposure?"auto":"manual",green/samples,history/samples,motion_diff/samples);
    assert(fabsf(green/samples-1.5f*(.38f+.23f*3/7))<.04f);
    assert(history/samples>.002f && motion_diff/samples>.002f);
    const char *dump=getenv("DM_TEST_TEMPORAL_DUMP");
    if (dump && !auto_exposure) {
        FILE *file=fopen(dump,"wb"); assert(file);
        assert(fwrite(pixels,sizeof *pixels,4*NP*MODES,file)==4*NP*MODES);
        assert(fclose(file)==0);
    }
    mtl_tex_release(&col); mtl_tex_release(&dep); mtl_tex_release(&vel); mtl_tex_release(&out); mtl_tex_release(&exp);
    mtl_queue_forget(col.serial); mtl_pool_del(mem); free(mem);
}
static void dlss_temporal_corpus(void) {
    dlss_temporal_sequence(0);
    dlss_temporal_sequence(1);
}
