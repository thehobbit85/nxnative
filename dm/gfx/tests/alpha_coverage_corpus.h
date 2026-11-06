// Draw the same five alpha values with coverage on, off and on again.
// A four-sample resolve exposes how many samples actually changed color.
static void alpha_coverage(prog_t *program, int samples) {
    enum { w=7, h=6 };
    tex_t msaa={.magic=MAGIC('T','E','X',0),.serial=0x81a0u+2*samples,
        .d={.width=w,.height=h,.depth=1,.levels=1,.samples=samples,.target=samples>1 ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    tex_t result=msaa;
    result.serial++; result.d.samples=1; result.d.target=NVN_TEXTURE_TARGET_2D;
    nvn_tex_layout(&msaa.d,&msaa.L); nvn_tex_layout(&result.d,&result.L);
    uint8_t *pool;
    assert(!posix_memalign((void **)&pool,getpagesize(),65536));
    memset(pool,0,65536); mtl_pool_add(pool,65536);
    uint8_t *pixels=pool+4096;
    list_t list={0};
    mtl_queue_activate(msaa.serial);
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&msaa});
    RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,w,h}});
    RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0,0,1,1});
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=program,.stages=3);
    for (int band=0;band<3;++band) {
        p_bind_state state={.kind=3};
        ms_st ms={.enable=1,.samples=samples,.a2c=band!=1};
        memcpy(state.bytes,&ms,sizeof ms); put(&list,OP_BIND_STATE,&state,sizeof state);
        for (int i=0;i<=4;++i) {
            float color[]={1,0,0,i*.25f};
            memcpy(pool+i*256,color,sizeof color);
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)(pool+i*256),.size=sizeof color);
            RECORD(OP_SCISSOR,p_scissor,.x=1+i,.y=2*band,.w=1,.h=2);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        }
    }
    RECORD(OP_COPY_T2T,p_copy_t2t,.src=&msaa,.dst=&result,.flags=DM_COPY_RESOLVE,
           .sr={.width=w,.height=h,.depth=1},.dr={.width=w,.height=h,.depth=1});
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&result,.src=(uintptr_t)pixels,.r={.width=w,.height=h,.depth=1});
    put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes);
    assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        uint8_t *p=pixels+(y*w+x)*4;
        int covered=(p[0]*4+127)/255, enabled=y<2 || y>=4;
        int want=0;
        if (x>0 && x<w-1) want=enabled ? x-1 : 4;
        if (samples==1 && enabled && x>=2 && x<=4) {
            // With one sample, intermediate alpha can resolve to either endpoint.
            if (covered!=0 && covered!=4) die("single sample resolved between endpoints at %d,%d",x,y);
            want=covered;
        }
        if (covered!=want || abs(4*p[0]-255*covered)>2 || p[1] || abs((p[0]+p[2])-255)>1)
            die("alpha coverage at %d,%d: red %u blue %u (%d of 4 samples), expected %d",x,y,p[0],p[2],covered,want);
    }
    mtl_tex_release(&msaa); mtl_tex_release(&result);
    mtl_pool_del(pool); free(pool); mtl_queue_forget(msaa.serial);
    printf("Alpha coverage: five alpha values, %d samples, enable/disable/re-enable and scissor guards passed\n",samples);
}
