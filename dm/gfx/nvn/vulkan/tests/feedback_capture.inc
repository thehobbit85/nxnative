static void feedback_capture(void) {
    // The shorter second stream fits ten vertices. Three triangles fit;
    // the following triangle must preserve its partial capacity and guards.
    struct { uint32_t a[96], b[24], saved[8], ended[8], copied[72], invocations; } memory;
    memset(&memory, 0xa5, sizeof memory);
    mtl_pool_add(&memory, sizeof memory);
    memory.invocations=0;
    tex_t target={.serial=0x82cb,.d={.width=4,.height=4,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&target.d,&target.L); mtl_queue_activate(target.serial);
    uint8_t pixels[16][4]={0};
    list_t list = {0};
    RECORD(OP_SET_RT, p_set_rt, .n=0);
    RECORD(OP_RAST_DISCARD, p_rast_discard, .on=1);
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=NULL, .stages=30);
    RECORD(OP_BIND_PROGRAM, p_bind_program, .prog=&capture_program, .stages=1);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=0,.index=3,.addr=(uintptr_t)&memory.invocations,.size=4);
    RECORD(OP_TF_BIND, p_tf_bind, .index=0,.addr=(uintptr_t)&memory.a[2],.size=80*4);
    RECORD(OP_TF_BIND, p_tf_bind, .index=2,.addr=(uintptr_t)&memory.b[2],.size=20*4);
    RECORD(OP_TF_STATE, p_tf_state, .what=0);
    RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.first=5,.count=3,.instances=2,.base_instance=7);
    RECORD(OP_TF_STATE, p_tf_state, .what=2,.ctl=(uintptr_t)memory.saved);
    RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_POINTS,.first=1,.count=1,.instances=1);
    RECORD(OP_TF_STATE, p_tf_state, .what=3,.ctl=(uintptr_t)memory.saved);
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
    RECORD(OP_RAST_DISCARD,p_rast_discard,.on=0);
    RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,4,4}});
    RECORD(OP_SCISSOR,p_scissor,.w=4,.h=4);
    RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0,0,1,1});
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&depth_range_program,.stages=2);
    RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.first=33,.count=3,.instances=1);
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1});
    RECORD(OP_DRAW, p_draw, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.first=77,.count=3,.instances=1);
    RECORD(OP_TF_STATE, p_tf_state, .what=1,.ctl=(uintptr_t)memory.ended);
    // Public DrawTransformFeedback records an indirect draw at control + 16.
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.ind=(uintptr_t)&memory.ended[4]);
    uint64_t copy[3] = {(uintptr_t)memory.a,(uintptr_t)memory.copied,sizeof memory.copied};
    put(&list, OP_COPY_B2B, copy, sizeof copy);
    put(&list, OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
    const uint32_t expected_state[8] = {6*5*4,0,6*2*4,0,6,1,0,0};
    assert(!memcmp(memory.saved, expected_state, sizeof expected_state));
    const uint32_t ended_state[8] = {9*5*4,0,9*2*4,0,9,1,0,0};
    assert(!memcmp(memory.ended, ended_state, sizeof ended_state));
    assert(memory.invocations == 6+1+3+3+9);
    for (unsigned k=0;k<16;k++) assert(pixels[k][0]==128 && pixels[k][2]==0);
    for (unsigned k=0; k<9; k++) {
        unsigned v=k<6 ? 5+k%3 : 33+k-6, i=k<6 ? 7+k/3 : 0; float f=(float)v;
        const uint32_t *a=memory.a+2+5*k, *b=memory.b+2+2*k;
        assert(a[0]==v && a[1]==i && a[2]==(0xf1230000u|v));
        assert(!memcmp(a+3,&f,4) && a[4]==v);
        assert(b[0]==v && b[1]==i);
    }
    for (unsigned k=0;k<96;k++) if(k<2 || k>=47) assert(memory.a[k]==0xa5a5a5a5u);
    for (unsigned k=0;k<24;k++) if(k<2 || k>=20) assert(memory.b[k]==0xa5a5a5a5u);
    assert(!memcmp(memory.copied,memory.a,sizeof memory.copied));
    mtl_queue_forget(target.serial); mtl_tex_release(&target); mtl_pool_del(&memory);
    puts("Transform feedback: two interleaved streams, repeated attributes, instances, whole primitive clipping and saved counts passed");
}
