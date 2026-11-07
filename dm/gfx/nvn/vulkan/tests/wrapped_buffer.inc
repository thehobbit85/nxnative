static void wrapped_buffer(void) {
    enum { COUNT=16389, SLOT=1580 };
    uint32_t *memory=malloc((COUNT+2)*4), result[2][4][4];assert(memory);
    mtl_pool_add(memory,(COUNT+2)*4);mtl_pool_add(result,sizeof result);mtl_queue_activate(SLOT);
    tex_t texture={.serial=0x179d0,.mem=(uint8_t *)(memory+1),
        .d={.width=COUNT,.height=1,.depth=1,.levels=1,.format=NVN_FORMAT_R32UI,.target=NVN_TEXTURE_TARGET_BUFFER}};
    nvn_tex_layout(&texture.d,&texture.L);mtl_reg_tex(SLOT,&texture,NULL);
    tex_t ordinary={.serial=0x179d1,.d={.width=1,.height=1,.depth=1,.levels=1,
        .format=NVN_FORMAT_R32UI,.target=NVN_TEXTURE_TARGET_2D}};
    nvn_tex_layout(&ordinary.d,&ordinary.L);mtl_reg_tex(SLOT+1,&ordinary,NULL);
    uint32_t pixel=42;
    const unsigned positions[]={0,16383,16384,16388};
    for (unsigned run=0;run<2;run++) {
        for (unsigned i=0;i<COUNT+2;i++) memory[i]=i+17;
        memset(result,0,sizeof result);list_t list={0};
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&ordinary,.src=(uintptr_t)&pixel,.r={.width=1,.height=1,.depth=1});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&wrapped_buffer_program,.stages=0x20);
        RECORD(OP_BIND_TEX,p_bind_handles,.stage=5,.first=0,.n=2,.h={SLOT,SLOT+1});
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=1,.addr=(uintptr_t)(memory+1),.size=COUNT*4);
        for (unsigned dispatch=0;dispatch<2;dispatch++) {
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)result[dispatch],.size=sizeof result[dispatch]);
            RECORD(OP_DISPATCH,p_dispatch,.x=4,.y=1,.z=1);
        }
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (unsigned dispatch=0;dispatch<2;dispatch++) for (unsigned i=0;i<4;i++) {
            uint32_t expected[]={positions[i]+18+dispatch*(100+i),16384,2,43};
            uint32_t *actual=result[dispatch][i];
            if (memcmp(actual,expected,sizeof expected)) die("wrapped buffer run%u dispatch%u lane%u: %u,%u,%u,%u expected %u,%u,%u,%u",
                run,dispatch,i,actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);
            assert(memory[1+positions[i]]==positions[i]+18+2*(100+i));
        }
        assert(memory[0]==17 && memory[COUNT+1]==COUNT+18);
    }
    mtl_reg_tex(SLOT,NULL,NULL);mtl_tex_release(&texture);mtl_queue_forget(SLOT);
    mtl_reg_tex(SLOT+1,NULL,NULL);mtl_tex_release(&ordinary);
    mtl_pool_del(memory);mtl_pool_del(result);free(memory);
    puts("Wrapped 2D buffer fetches preserve row boundaries, size queries, ordinary images and ordered storage aliases");
}
