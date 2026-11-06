// The four bits of the guest operation select the truth table entries for
// (source,destination) = (1,1), (1,0), (0,1), (0,0) in order.
static uint32_t logic_word(unsigned op, uint32_t src, uint32_t dst) {
    return ((op&1)?src&dst:0) | ((op&2)?src&~dst:0) | ((op&4)?~src&dst:0) | ((op&8)?~src&~dst:0);
}
static void logic_operations(void) {
    const struct { int fmt, width, channels, integer; } cases[]={
        {NVN_FORMAT_RGBA8,1,4,0}, {NVN_FORMAT_RGBA16,2,4,0},
        {NVN_FORMAT_RG32UI,4,2,1}, {NVN_FORMAT_RGBA8UI,1,4,1},
    };
    const uint32_t src[]={0x8926a396,0x54a5cc36,0x75d3c30f,0x8aa65c1b};
    const uint32_t dst[]={0x12533c65,0x96569253,0x27816346,0x587be3a4};
    for (int trial=0;trial<6;++trial) {
        int t=trial<4 ? trial : trial-2; // Repeat both UINT targets with a float-declared Maxwell output.
        int width=cases[t].width, channels=cases[t].channels;
        uint32_t mask=UINT32_MAX>>(32-8*width), uniform[4];
        uint8_t seed[64]={0};
        size_t pixel_bytes=channels*width;
        for (int c=0;c<4;++c) {
            uniform[c]=src[c]&mask;
            if (!cases[t].integer) { float value=(float)uniform[c]/mask; memcpy(&uniform[c],&value,4); }
        }
        for (int x=0;x<4;++x) for (int c=0;c<channels;++c)
            memcpy(seed+x*pixel_bytes+c*width,&dst[c],width);
        tex_t target={.serial=0x8180u+t,.d={.width=4,.height=1,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=cases[t].fmt}};
        nvn_tex_layout(&target.d,&target.L); mtl_queue_activate(target.serial);
        for (unsigned op=0;op<16;++op) {
            list_t list={0}; uint8_t pixels[64]={0};
            RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&target,.src=(uintptr_t)seed,.r={.width=4,.height=1,.depth=1});
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
            RECORD(OP_SCISSOR,p_scissor,.x=1,.w=2,.h=1);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&logic_program[trial<4 && cases[t].integer],.stages=3);
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)uniform,.size=sizeof uniform);
            // Non-COPY logic replaces blending. Both blend factors zero make
            // this observable, including on an integer attachment.
            p_bind_state state={.kind=0};
            blend_st blend={.src_rgb=1,.dst_rgb=1,.src_a=1,.dst_a=1,.eq_rgb=1,.eq_a=1};
            memcpy(state.bytes,&blend,sizeof blend); put(&list,OP_BIND_STATE,&state,sizeof state);
            color_st color={.logic_op=op,.blend_en=op!=NVN_LOGIC_OP_COPY,.alpha_func=NVN_ALPHA_FUNC_ALWAYS};
            state.kind=1; memcpy(state.bytes,&color,sizeof color); put(&list,OP_BIND_STATE,&state,sizeof state);
            unsigned write_mask=(op&1)?15:5;
            chmask_st cm={.mask=write_mask}; state.kind=2; memcpy(state.bytes,&cm,sizeof cm); put(&list,OP_BIND_STATE,&state,sizeof state);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)pixels,.r={.width=4,.height=1,.depth=1});
            put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (int x=0;x<4;++x) for (int c=0;c<channels;++c) {
                uint32_t got=0,want=dst[c]&mask;
                if ((x==1||x==2) && (write_mask>>c&1)) {
                    unsigned effective=getenv("DM_VK_NO_LOGIC_OP") ? NVN_LOGIC_OP_COPY : op;
                    want=logic_word(effective,src[c],dst[c])&mask;
                }
                memcpy(&got,pixels+x*pixel_bytes+c*width,width);
                if (got!=want) die("logic op %u format %#x pixel %d channel %d: %#x expected %#x",op,cases[t].fmt,x,c,got,want);
            }
        }
        mtl_tex_release(&target); mtl_queue_forget(target.serial);
    }
    puts("Vulkan logic operations: 16 truth tables on four integer and normalized formats, scissor and masks passed");
}

// An attached floating-point target in the gap between two shader outputs
// must keep its data. Its format cannot disable the neighboring XOR writes.
static void sparse_mrt_outputs(void) {
    enum { width=4 };
    tex_t rt[3];
    uint32_t seed[3][width], result[3][width];
    const uint32_t colors[]={0xff4080bf,0,0x00bf8040};
    list_t list={0};
    for (unsigned i=0;i<3;++i) {
        rt[i]=(tex_t){.serial=0x81b0u+i,.d={.width=width,.height=1,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=i==1 ? NVN_FORMAT_R32F : NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&rt[i].d,&rt[i].L);
        for (unsigned x=0;x<width;++x) seed[i][x]=0x3f123456u+16*i+x;
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&rt[i],.src=(uintptr_t)seed[i],.r={.width=width,.height=1,.depth=1});
    }
    RECORD(OP_SET_RT,p_set_rt,.n=3,.color={&rt[0],&rt[1],&rt[2]});
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&sparse_mrt_program,.stages=3);
    p_bind_state state={.kind=1}; color_st color={.blend_en=7,.logic_op=NVN_LOGIC_OP_XOR,.alpha_func=NVN_ALPHA_FUNC_ALWAYS};
    memcpy(state.bytes,&color,sizeof color); put(&list,OP_BIND_STATE,&state,sizeof state);
    RECORD(OP_SCISSOR,p_scissor,.x=1,.w=2,.h=1);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    for (unsigned i=0;i<3;++i)
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&rt[i],.src=(uintptr_t)result[i],.r={.width=width,.height=1,.depth=1});
    put(&list,OP_END,NULL,0); mtl_queue_activate(rt[0].serial);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int i=0;i<3;++i) for (int x=0;x<width;++x) {
        uint32_t expected=seed[i][x];
        if (i!=1 && x>=1 && x<=2) expected=getenv("DM_VK_NO_LOGIC_OP") ? colors[i] : expected^colors[i];
        if (result[i][x]!=expected) die("sparse MRT %d at %d: %#x expected %#x",i,x,result[i][x],expected);
    }
    for (int i=0;i<3;++i) mtl_tex_release(&rt[i]);
    mtl_queue_forget(rt[0].serial);
    puts("Vulkan sparse MRT: output locations zero and two applied logic, unwritten float attachment preserved");
}

// One Maxwell program can bind the same raw color registers to either class
// of integer target while color zero remains floating (and alpha-tested).
static void mixed_integer_outputs(void) {
    const uint32_t uniform[4]={0x80000011,0x7fc00033,0x00200044,0xdeadbeef};
    const int formats[]={NVN_FORMAT_RGBA32UI,NVN_FORMAT_RGBA32I};
    for (unsigned trial=0;trial<2;++trial) {
        tex_t color={.serial=0x81c0u+4*trial,.d={.width=4,.height=2,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}}, integer=color;
        integer.serial++; integer.d.format=formats[trial];
        nvn_tex_layout(&color.d,&color.L); nvn_tex_layout(&integer.d,&integer.L);
        uint32_t pixels[8][4]={{0}}, rgba[8]={0};
        list_t list={0};
        RECORD(OP_SET_RT,p_set_rt,.n=2,.color={&color,&integer});
        RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=15,.rgba={0,0,1,1});
        RECORD(OP_CLEAR_COLOR,p_clear_color,.index=1,.mask=15);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&mixed_color_program,.stages=3);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)uniform,.size=sizeof uniform);
        RECORD(OP_ALPHA_REF,p_alpha_ref,.ref=.5f);
        for (int y=0;y<2;++y) {
            p_bind_state state={.kind=1};
            color_st cs={.logic_op=NVN_LOGIC_OP_COPY,.alpha_func=y ? NVN_ALPHA_FUNC_GREATER : NVN_ALPHA_FUNC_ALWAYS};
            memcpy(state.bytes,&cs,sizeof cs); put(&list,OP_BIND_STATE,&state,sizeof state);
            RECORD(OP_SCISSOR,p_scissor,.y=y,.w=4,.h=1);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        }
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)rgba,.r={.width=4,.height=2,.depth=1});
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&integer,.src=(uintptr_t)pixels,.r={.width=4,.height=2,.depth=1});
        put(&list,OP_END,NULL,0); mtl_queue_activate(color.serial);
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (int i=0;i<8;++i) {
            int pass=i<4 || (i&1);
            uint32_t want=pass ? (i&1 ? 0xbf0000ff : 0x400000ff) : 0xffff0000;
            if (rgba[i]!=want) die("mixed integer trial %u normalized pixel %d: %#x expected %#x",trial,i,rgba[i],want);
            for (int c=0;c<4;++c) {
                want=pass ? uniform[c==2 && !(i&1) ? 0 : c] : 0;
                if (pixels[i][c]!=want) die("mixed integer trial %u pixel %d channel %d: %#x expected %#x",
                    trial,i,c,pixels[i][c],want);
            }
        }
        mtl_tex_release(&integer); mtl_tex_release(&color); mtl_queue_forget(color.serial);
    }
    puts("Vulkan mixed integer targets: raw signed and unsigned words, partial stores, alpha and normalized output passed");
}
