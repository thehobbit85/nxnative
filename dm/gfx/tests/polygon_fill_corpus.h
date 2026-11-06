// A triangle entirely inside the viewport distinguishes wireframe from fill.
// Check edge and interior regions away from the implementation-specific rule
// for line end points. Culling must still classify the original triangle.
static void polygon_fill(prog_t *program, int native_lines) {
    enum { w=16, h=16 };
    tex_t color={.magic=MAGIC('T','E','X',0),.serial=0x81e0,
        .d={.width=w,.height=h,.depth=1,.levels=1,.samples=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
    nvn_tex_layout(&color.d,&color.L);
    float *pixels;
    assert(!posix_memalign((void **)&pixels,getpagesize(),65536));
    memset(pixels,0,65536); mtl_pool_add(pixels,65536);
    mtl_queue_activate(color.serial);
    mtl_set_origin_lower_left(0); mtl_set_queue_origin_lower_left(0);
    const unsigned modes[]={NVN_POLYGON_MODE_FILL,NVN_POLYGON_MODE_LINE,NVN_POLYGON_MODE_POINT};
    for (unsigned mode=0;mode<3;++mode) for (int front=0;front<2;++front) {
        int survivors=0;
        for (int cull=0;cull<4;++cull) {
            list_t list={0};
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color});
            RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,w,h}});
            RECORD(OP_SCISSOR,p_scissor,.w=w,.h=h);
            RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=program,.stages=3);
            p_bind_state state={.kind=4};
            poly_st poly={.mode=modes[mode],.front=front,.cull=cull};
            memcpy(state.bytes,&poly,sizeof poly); put(&list,OP_BIND_STATE,&state,sizeof state);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)pixels,.r={.width=w,.height=h,.depth=1});
            put(&list,OP_END,NULL,0);
            mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            int count=0;
            for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
                if (pixels[y*w+x]==0) continue;
                if (pixels[y*w+x]!=.25f || x<2 || x>13 || y<2 || y>13)
                    die("polygon mode %u cull %d unexpectedly wrote %g at %d,%d",modes[mode],cull,pixels[y*w+x],x,y);
                ++count;
            }
            if (!cull && !count) die("polygon without culling disappeared");
            if (cull==NVN_FACE_FRONT_AND_BACK && count) die("fully culled polygon drew %d pixels",count);
            if (cull==NVN_FACE_BACK || cull==NVN_FACE_FRONT) survivors+=(count!=0);
            if (!count) continue;
            // The center is well inside the triangle under either clip origin.
            int lines=modes[mode]==NVN_POLYGON_MODE_LINE && native_lines;
            if ((pixels[7*w+8]!=0)==lines || count<16)
                die("polygon mode %u cull %d coverage %d center %g",modes[mode],cull,count,pixels[7*w+8]);
            if (cull==NVN_FACE_NONE) memcpy(pixels+w*h,pixels,sizeof(float)*w*h);
            else if (memcmp(pixels+w*h,pixels,sizeof(float)*w*h)) die("culling changed the surviving triangle");
        }
        if (survivors!=1) die("expected one winding to survive, mode %u front %d",modes[mode],front);
    }
    mtl_tex_release(&color); mtl_pool_del(pixels); free(pixels); mtl_queue_forget(color.serial);
    puts("Polygon fill: line, solid and reference point mode, both windings and all cull modes passed");
}
