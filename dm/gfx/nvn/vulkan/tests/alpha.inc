// Match the Metal alpha fixture's ramp and references. Read color and depth
// independently, including early fragment tests and pre-discard storage writes.
static int alpha_passes(unsigned function, float alpha, float reference) {
    switch (function) {
        case NVN_ALPHA_FUNC_NEVER: return 0;
        case NVN_ALPHA_FUNC_LESS: return alpha < reference;
        case NVN_ALPHA_FUNC_EQUAL: return alpha == reference;
        case NVN_ALPHA_FUNC_LEQUAL: return alpha <= reference;
        case NVN_ALPHA_FUNC_GREATER: return alpha > reference;
        case NVN_ALPHA_FUNC_NOTEQUAL: return alpha != reference;
        case NVN_ALPHA_FUNC_GEQUAL: return alpha >= reference;
        default: return 1;
    }
}
static void alpha_references(void) {
    tex_t color={.serial=0x8150,.d={.width=8,.height=6,.depth=1,.samples=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    tex_t depth=color; depth.serial++; depth.d.format=NVN_FORMAT_DEPTH32F;
    nvn_tex_layout(&color.d,&color.L); nvn_tex_layout(&depth.d,&depth.L);
    const float refs[]={0,.3125f,.5f,1,NAN,.5f}, width=8;
    mtl_queue_activate(color.serial);
    for (unsigned bound=0; bound<2; ++bound) {
        uint64_t libraries, before, after;
        mtl_create_stats(&libraries,&before);
        for (unsigned f=1; f<=8; ++f) {
            uint8_t pixels[6][8][4]={0};
            float z[6][8]={0};
            uint32_t writes=0;
            list_t list={0};
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color},.depth=&depth);
            RECORD(OP_SCISSOR,p_scissor,.w=8,.h=6);
            RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0,0,1,1});
            RECORD(OP_CLEAR_DS,p_clear_ds,.depth=1,.depth_mask=1);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&alpha_program[bound],.stages=3);
            if (bound) {
                RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)&width,.size=sizeof width);
                RECORD(OP_BIND_SSBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)&writes,.size=sizeof writes);
            }
            p_bind_state state={.kind=5};
            ds_st ds={.dt=1,.dw=1,.df=NVN_COMPARE_FUNC_LESS};
            memcpy(state.bytes,&ds,sizeof ds); put(&list,OP_BIND_STATE,&state,sizeof state);
            state.kind=1;
            color_st cs={.logic_op=NVN_LOGIC_OP_COPY,.alpha_func=f};
            memcpy(state.bytes,&cs,sizeof cs); put(&list,OP_BIND_STATE,&state,sizeof state);
            for (int row=0; row<6; ++row) {
                RECORD(OP_ALPHA_REF,p_alpha_ref,.ref=refs[row]);
                RECORD(OP_SCISSOR,p_scissor,.y=row,.w=8,.h=1);
                RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            }
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)pixels,.r={.width=8,.height=6,.depth=1});
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&depth,.src=(uintptr_t)z,.r={.width=8,.height=6,.depth=1});
            put(&list,OP_END,NULL,0);
            mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (int row=0; row<6; ++row) for (int x=0; x<8; ++x) {
                float alpha=(x+.5f)/8;
                int pass=alpha_passes(f,alpha,refs[row]);
                uint8_t *p=pixels[row][x];
                if (p[0]!=(pass?255:0) || p[2]!=(pass?0:255) || z[row][x]!=((bound||pass)?.25f:1))
                    die("alpha bound %u func %u ref %g pixel %d: red %u blue %u depth %g expected pass %d",
                        bound,f,refs[row],x,p[0],p[2],z[row][x],pass);
            }
            if (bound) assert(writes==48);
        }
        mtl_create_stats(&libraries,&after);
        assert(after-before==2); // one ordinary and one alpha-tested pipeline, across all functions and references
    }
    // ColorState NEVER also reaches integer-only passes. All integer
    // channels must still be written, even when the reference rejects floats.
    tex_t integer=color; integer.serial=0x8152; integer.d.format=NVN_FORMAT_RGBA8UI;
    nvn_tex_layout(&integer.d,&integer.L); list_t list={0};
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&integer});
    RECORD(OP_SCISSOR,p_scissor,.w=8,.h=6);
    RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&alpha_program[2],.stages=3);
    p_bind_state state={.kind=1}; color_st cs={.logic_op=NVN_LOGIC_OP_COPY,.alpha_func=NVN_ALPHA_FUNC_NEVER};
    memcpy(state.bytes,&cs,sizeof cs); put(&list,OP_BIND_STATE,&state,sizeof state);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    uint8_t pixels[48][4];
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&integer,.src=(uintptr_t)pixels,.r={.width=8,.height=6,.depth=1});
    put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int i=0;i<48;++i) assert(!memcmp(pixels[i],(uint8_t[]){17,32,64,128},4));
    mtl_tex_release(&integer);
    mtl_tex_release(&color); mtl_tex_release(&depth); mtl_queue_forget(color.serial);
    puts("Vulkan alpha tests: 768 ramp pixels, NaN, early/late depth, storage writes and pipeline reuse passed");
}
