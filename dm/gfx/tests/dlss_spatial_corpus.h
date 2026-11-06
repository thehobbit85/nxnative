// Force spatial reconstruction by omitting motion/depth. A two-axis ramp
// exposes sampling at the wrong texel center or with the wrong input size.
static void dlss_spatial_corpus(int output_subrect) {
    tex_t src={.magic=MAGIC('T','E','X',0),.serial=0x82f0,.d={.width=4,.height=4,.depth=1,.samples=1,
        .levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
    tex_t dst=src; dst.serial++; dst.d.width=dst.d.height=8;
    nvn_tex_layout(&src.d,&src.L); nvn_tex_layout(&dst.d,&dst.L);
    float *input; assert(!posix_memalign((void **)&input,getpagesize(),65536));
    float *pixels=input+64;
    mtl_pool_add(input,65536); mtl_queue_activate(src.serial);
    for (unsigned y=0;y<4;y++) for (unsigned x=0;x<4;x++) input[y*4+x]=8*y+2*x;
    for (int choice=0;choice<2+output_subrect;choice++) {
        int base=choice!=0, span=base?2:4, dx=choice==2?1:0, dy=choice==2?2:0;
        int w=choice==2?5:8, h=choice==2?3:8;
        list_t list={0};
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&src,.src=(uintptr_t)input,.r={.width=4,.height=4,.depth=1});
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&dst});
        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={-9,0,0,1});
        RECORD(OP_DLSS,p_dlss,.tex={[DLSS_COLOR]=&src,[DLSS_OUTPUT]=&dst},.net=44,
            .iw=base?span:0,.ih=base?span:0,.ow=w,.oh=h,
            .base={[DLSS_COLOR]={base,base},[DLSS_OUTPUT]={dx,dy}});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&dst,.src=(uintptr_t)pixels,.r={.width=8,.height=8,.depth=1});
        put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (int y=0;y<8;y++) for (int x=0;x<8;x++) {
            float want=-9;
            if (x>=dx && x<dx+w && y>=dy && y<dy+h) {
                float u=base+((x-dx+.5f)*span/w)-.5f, v=base+((y-dy+.5f)*span/h)-.5f;
                want=2*fminf(3,fmaxf(0,u))+8*fminf(3,fmaxf(0,v));
            }
            if (!(fabsf(pixels[y*8+x]-want)<=.04f)) {
                fprintf(stderr,"DLSS spatial case %d pixel %d,%d got %g expected %g\n",choice,x,y,pixels[y*8+x],want);
                abort();
            }
        }
    }
    mtl_tex_release(&src); mtl_tex_release(&dst); mtl_queue_forget(src.serial);
    mtl_pool_del(input); free(input);
    puts("DLSS spatial fallback: bilinear texel centers, HDR values, input rectangles and guarded outputs passed");
}
