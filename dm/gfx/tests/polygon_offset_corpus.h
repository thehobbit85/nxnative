// First store the sloped triangle's unbiased depth. A second coplanar draw
// writes color only if its signed offset passes the strict comparison. Read
// depth separately to distinguish a clamped bias from a merely nonzero one.
static void polygon_offset(prog_t *program) {
    enum { w=8, h=8 };
    tex_t color = {.magic=MAGIC('T','E','X',0),.serial=0x8140,
        .d={.width=w,.height=h,.depth=1,.levels=1,.samples=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
    tex_t depth = color; depth.serial++; depth.d.format=NVN_FORMAT_DEPTH32F;
    nvn_tex_layout(&color.d,&color.L); nvn_tex_layout(&depth.d,&depth.L);
    float *pixels;
    assert(!posix_memalign((void **)&pixels,getpagesize(),65536));
    memset(pixels,0,65536); mtl_pool_add(pixels,65536);
    const p_poly_offset cases[] = {
        {.units=32}, {.units=-32}, {.factor=2}, {.factor=-2},
        {.factor=2,.clamp=.015625f}, {.factor=-2,.clamp=-.015625f},
    };
    mtl_queue_activate(color.serial);
    for (unsigned i=0; i<sizeof cases/sizeof *cases; ++i) {
        list_t list={0};
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color},.depth=&depth);
        RECORD(OP_SCISSOR,p_scissor,.w=w,.h=h);
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,w,h}});
        RECORD(OP_DEPTH_RANGE,p_depth_range,.n=0,.f=1);
        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={-1,0,0,0});
        RECORD(OP_CLEAR_DS,p_clear_ds,.depth=1,.depth_mask=1);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=program,.stages=3);
        p_bind_state mask={.kind=2}, ds={.kind=5}, poly={.kind=4};
        ds_st test={.dt=1,.dw=1,.df=NVN_COMPARE_FUNC_ALWAYS};
        poly_st raster={.mode=NVN_POLYGON_MODE_FILL};
        memcpy(ds.bytes,&test,sizeof test); memcpy(poly.bytes,&raster,sizeof raster);
        put(&list,OP_BIND_STATE,&mask,sizeof mask);
        put(&list,OP_BIND_STATE,&ds,sizeof ds);
        put(&list,OP_BIND_STATE,&poly,sizeof poly);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        mask.bytes[0]=15; put(&list,OP_BIND_STATE,&mask,sizeof mask);
        test.df=(i&1) ? NVN_COMPARE_FUNC_LESS : NVN_COMPARE_FUNC_GREATER;
        memcpy(ds.bytes,&test,sizeof test); put(&list,OP_BIND_STATE,&ds,sizeof ds);
        put(&list,OP_POLY_OFFSET,&cases[i],sizeof cases[i]);
        for (int y=0; y<h; ++y) {
            raster.offset_en=y;
            memcpy(poly.bytes,&raster,sizeof raster); put(&list,OP_BIND_STATE,&poly,sizeof poly);
            RECORD(OP_SCISSOR,p_scissor,.y=y,.w=w,.h=1);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        }
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)pixels,.r={.width=w,.height=h,.depth=1});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&depth,.src=(uintptr_t)(pixels+w*h),.r={.width=w,.height=h,.depth=1});
        put(&list,OP_END,NULL,0);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (int y=0; y<h; ++y) for (int x=0; x<w; ++x) {
            float base=.25f+(x+.5f)/32, z=pixels[w*h+y*w+x], c=pixels[y*w+x];
            int enabled=!!(y & NVN_POLYGON_OFFSET_ENABLE_FILL);
            if ((c != -1) != enabled)
                die("polygon offset case %u mask %d pixel %d color %g depth %.9g unbiased %.9g",i,y,x,c,z,base);
            float delta=z-base;
            if (!enabled && delta!=0) die("disabled depth offset %.9g",delta);
            if (!enabled) continue;
            if (i<2) {
                // The largest vertex depth is .75, so a D32F unit is 2^-24.
                float expected=cases[i].units*0x1p-24f;
                if (delta!=expected) die("constant depth offset %.9g expected %.9g",delta,expected);
            } else {
                int clamped=cases[i].clamp && !getenv("DM_VK_NO_DEPTH_BIAS_CLAMP");
                float expected=clamped ? cases[i].clamp : cases[i].factor/32;
                if (delta!=expected) die("polygon slope or clamp case %u depth %.9g expected %.9g",i,z,base+expected);
            }
        }
    }
    mtl_tex_release(&depth); mtl_tex_release(&color);
    mtl_pool_del(pixels); free(pixels); mtl_queue_forget(0x8140);
    puts("Polygon offsets: all eight masks, signed units, slope, clamp and coplanar depth comparisons passed");
}
