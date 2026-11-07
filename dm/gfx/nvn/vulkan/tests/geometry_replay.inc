static void geometry_replay(void) {
    tex_t target={.serial=0x17980,.d={.width=4,.height=4,.depth=1,.levels=1,
        .format=NVN_FORMAT_RGBA8,.target=NVN_TEXTURE_TARGET_2D}};
    nvn_tex_layout(&target.d,&target.L);mtl_queue_activate(target.serial);
    const float triangle[][4]={{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}};
    float vertices[195][4];uint16_t indices[195];
    for (unsigned i=0;i<195;i++) { memcpy(vertices[i],triangle[i%3],16);indices[i]=i%3; }
    mtl_pool_add(indices,sizeof indices);
    const uint32_t sparse_indices[]={0,3,1u<<20};
    size_t sparse_bytes=((1u<<20)+6u)*16u;
    float (*sparse_vertices)[4]=malloc(sparse_bytes);assert(sparse_vertices);
    for (unsigned i=0;i<3;i++) memcpy(sparse_vertices[sparse_indices[i]+5],triangle[i],16);
    mtl_pool_add((void *)sparse_indices,sizeof sparse_indices);
    for (unsigned test=0;test<24;test++) {
        unsigned mode=test%6;
        int feedback=test>=6, discard=test>=12 && test<18, sparse=test>=18;
        struct { float a[48], b[24];uint32_t control[8]; } output;
        memset(&output,0xa5,sizeof output);mtl_pool_add(&output,sizeof output);
        uint8_t pixels[4*4*4];
        list_t list={0};
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,4,4}});
        RECORD(OP_SCISSOR,p_scissor,.w=4,.h=4);
        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0,0,1,1});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&geometry_program,.stages=7);
        RECORD(OP_RAST_DISCARD,p_rast_discard,.on=discard);
        if (feedback) {
            RECORD(OP_TF_BIND,p_tf_bind,.index=0,.addr=(uintptr_t)&output.a[2],.size=44*4);
            RECORD(OP_TF_BIND,p_tf_bind,.index=2,.addr=(uintptr_t)&output.b[2],.size=20*4);
            RECORD(OP_TF_STATE,p_tf_state,.what=0);
        }
        p_bind_vattr attributes={.n=1};va_st attribute={.fmt=NVN_FORMAT_RGBA32F};
        memcpy(attributes.st,&attribute,sizeof attribute);put(&list,OP_BIND_VATTR,&attributes,sizeof attributes);
        RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=1,.st={{.stride=16}});
        RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=sparse ? sparse_bytes : sizeof vertices,
            .addr=sparse ? (uintptr_t)sparse_vertices : (uintptr_t)vertices);
        uint32_t arguments[]={sparse || mode<2 ? 3u : 195u,mode<4 ? 1u : 2u,0,sparse ? 5u : 0u,7};
        mtl_pool_add(arguments,sizeof arguments);
        if (mode&1 || sparse) {
            RECORD(OP_DRAW_IDX,p_draw_idx,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=arguments[0],.itype=sparse ? 2 : 1,
                .ibuf=sparse ? (uintptr_t)sparse_indices : (uintptr_t)indices,.base_vertex=sparse ? 5 : 0,
                .instances=arguments[1],.base_instance=7,.ind=mode==5 ? (uintptr_t)arguments : 0);
        } else {
            arguments[3]=7;
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=arguments[0],.instances=arguments[1],
                .base_instance=7,.ind=mode==4 ? (uintptr_t)arguments : 0);
        }
        if (feedback) RECORD(OP_TF_STATE,p_tf_state,.what=1,.ctl=(uintptr_t)output.control);
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        mtl_pool_del(arguments);
        for (unsigned i=0;i<16;i++) if (memcmp(pixels+4*i,(uint8_t[]){discard?0:255,0,discard?255:0,255},4))
            die("geometry passthrough test%u pixel%u: %u,%u,%u,%u",test,i,pixels[4*i],pixels[4*i+1],pixels[4*i+2],pixels[4*i+3]);
        if (feedback) {
            unsigned count=sparse ? 3*arguments[1] : mode<2 ? 3 : 9;
            const uint32_t state[]={count*16,0,count*8,0,count,1,0,0};
            assert(!memcmp(output.control,state,sizeof state));
            for (unsigned i=0;i<count;i++) {
                const float expected[]={1,triangle[i%3][0],triangle[i%3][1],0};
                const float *actual=output.a+2+4*i;
                if (memcmp(actual,expected,sizeof expected)) die("geometry feedback test%u vertex%u: %g,%g,%g,%g expected %g,%g,%g,%g",
                    test,i,actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);
                assert(output.b[2+2*i]==1 && output.b[3+2*i]==1);
            }
            for (unsigned i=0;i<48;i++) if (i<2 || i>=2+4*count)
                assert(!memcmp(output.a+i,(uint32_t[]){0xa5a5a5a5},4));
            for (unsigned i=0;i<24;i++) if (i<2 || i>=2+2*count)
                assert(!memcmp(output.b+i,(uint32_t[]){0xa5a5a5a5},4));
        }
        mtl_pool_del(&output);
    }
    mtl_pool_del((void *)sparse_indices);free(sparse_vertices);
    mtl_pool_del(indices);mtl_tex_release(&target);mtl_queue_forget(target.serial);
    puts("Maxwell geometry replay passed direct/indexed/indirect instances, multi-block compaction, sparse capture IDs and ordered two-stream feedback with raster discard and whole-primitive clipping");
}

static void geometry_split_feedback(void) {
    // The shared Maxwell fixture emits a triangle and a four-vertex strip,
    // with raw primitive/invocation/vertex IDs in location 0. Its nine-vertex
    // allocation emits seven vertices and expands to nine ordered records.
    enum { PRIMITIVES=65, INSTANCES=2, INVOCATIONS=3, RECORDS=PRIMITIVES*INSTANCES*INVOCATIONS*9 };
    struct { uint32_t guard[4], values[RECORDS][4], tail[4], control[8]; } *output=malloc(sizeof *output);
    assert(output);memset(output,0xa5,sizeof *output);mtl_pool_add(output,sizeof *output);
    float positions[PRIMITIVES*3][4];
    for (unsigned i=0;i<PRIMITIVES*3;i++) memcpy(positions[i],(float[]){0,0,0,1},16);
    list_t list={0};
    RECORD(OP_SET_RT,p_set_rt,.n=0);
    RECORD(OP_RAST_DISCARD,p_rast_discard,.on=1);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&geometry_explicit_program,.stages=7);
    p_bind_vattr attributes={.n=1};va_st attribute={.fmt=NVN_FORMAT_RGBA32F};
    memcpy(attributes.st,&attribute,sizeof attribute);put(&list,OP_BIND_VATTR,&attributes,sizeof attributes);
    RECORD(OP_BIND_VSTREAM,p_bind_vstream,.n=1,.st={{.stride=16}});
    RECORD(OP_BIND_VBUF1,p_bind_vbuf1,.index=0,.size=sizeof positions,.addr=(uintptr_t)positions);
    RECORD(OP_TF_BIND,p_tf_bind,.index=0,.addr=(uintptr_t)output->values,.size=sizeof output->values);
    RECORD(OP_TF_STATE,p_tf_state,.what=0);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=PRIMITIVES*3,.instances=INSTANCES,.base_instance=7);
    RECORD(OP_TF_STATE,p_tf_state,.what=1,.ctl=(uintptr_t)output->control);
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    const uint32_t expected_control[]={RECORDS*16,0,0,0,RECORDS,1,0,0};
    assert(!memcmp(output->control,expected_control,sizeof expected_control));
    const uint32_t emitted[]={0,1,2,3,4,5,5,4,6};
    unsigned record=0;
    for (unsigned instance=0;instance<INSTANCES;instance++)
        for (unsigned primitive=0;primitive<PRIMITIVES;primitive++)
            for (unsigned invocation=0;invocation<INVOCATIONS;invocation++)
                for (unsigned vertex=0;vertex<9;vertex++,record++) {
                    const uint32_t expected[]={primitive,invocation,emitted[vertex],0x3f800000};
                    const uint32_t *actual=output->values[record];
                    if (memcmp(actual,expected,sizeof expected)) die("geometry split record%u: %u,%u,%u,%x expected %u,%u,%u,%x",
                        record,actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);
                }
    for (unsigned i=0;i<4;i++) assert(output->guard[i]==0xa5a5a5a5 && output->tail[i]==0xa5a5a5a5);
    mtl_pool_del(output);free(output);
    puts("Geometry split feedback preserved primitive/invocation IDs, strip winding, GPU emitted counts and stream state");
}
