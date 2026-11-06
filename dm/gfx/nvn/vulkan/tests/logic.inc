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
    for (int t=0;t<4;++t) {
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
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&logic_program[cases[t].integer],.stages=3);
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
