static void wrapped_buffer_table(void) {
    enum { COUNT=40, SLOT=1600 };
    tex_t textures[COUNT];uint32_t memory[COUNT][COUNT], result[COUNT+1][4];
    uint64_t handles[COUNT];
    mtl_pool_add(memory,sizeof memory);mtl_pool_add(result,sizeof result);mtl_pool_add(handles,sizeof handles);
    for (unsigned i=0;i<COUNT;i++) {
        for (unsigned j=0;j<COUNT;j++) memory[i][j]=100+i+j;
        textures[i]=(tex_t){.serial=0x17a00+i,.mem=(uint8_t *)memory[i],
            .d={.width=i+1,.height=1,.depth=1,.levels=1,.format=NVN_FORMAT_R32UI,.target=NVN_TEXTURE_TARGET_BUFFER}};
        nvn_tex_layout(&textures[i].d,&textures[i].L);mtl_reg_tex(SLOT+i,&textures[i],NULL);
    }
    const unsigned live_counts[]={1,40,2,40};
    for (unsigned run=0;run<sizeof live_counts/sizeof *live_counts;run++) {
        unsigned live=live_counts[run];
        for (unsigned i=0;i<COUNT;i++) handles[i]=i<live ? SLOT+i : 0;
        memset(result,0,sizeof result);list_t list={0};
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&wrapped_table_program,.stages=0x20);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=7,.addr=(uintptr_t)handles,.size=sizeof handles);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=8,.addr=0,.size=0);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)result,.size=sizeof result);
        RECORD(OP_DISPATCH,p_dispatch,.x=COUNT+1,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (unsigned i=0;i<=COUNT;i++) {
            uint32_t expected[]={i<live ? 100+i : 0,i<live ? i+1 : 4096,1,1};
            if (memcmp(result[i],expected,16)) die("wrapped table live%u lane%u: %u,%u,%u,%u",live,i,
                result[i][0],result[i][1],result[i][2],result[i][3]);
        }
    }
    for (unsigned i=0;i<COUNT;i++) { mtl_reg_tex(SLOT+i,NULL,NULL);mtl_tex_release(&textures[i]); }
    mtl_pool_del(memory);mtl_pool_del(result);mtl_pool_del(handles);
}

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
    tex_t short_view=texture;short_view.serial++;short_view.d.width=17;
    short_view.mem=(uint8_t *)(memory+5);nvn_tex_layout(&short_view.d,&short_view.L);
    mtl_reg_tex(SLOT+2,&short_view,NULL);
    uint64_t handles[5]={0,0,SLOT,SLOT+2,0};
    mtl_pool_add(handles,sizeof handles);
    for (unsigned form=0;form<3;form++) {
        for (unsigned i=0;i<COUNT+2;i++) memory[i]=i+17;
        memset(result,0,sizeof result);list_t list={0};
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&wrapped_array_program[form],.stages=0x20);
        RECORD(OP_BIND_TEX,p_bind_handles,.stage=5,.first=0,.n=3,.h={SLOT,SLOT+2,0});
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=7,.addr=(uintptr_t)handles,.size=sizeof handles);
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)result,.size=sizeof result);
        RECORD(OP_DISPATCH,p_dispatch,.x=3,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        const uint32_t expected[3][4]={{16402,16384,2,1},{38,17,1,1},{0,4096,1,1}};
        for (unsigned lane=0;lane<3;lane++) if (memcmp(result[0][lane],expected[lane],16))
            die("wrapped array form%u lane%u: %u,%u,%u,%u",form,lane,
                result[0][lane][0],result[0][lane][1],result[0][lane][2],result[0][lane][3]);
    }
    mtl_pool_del(handles);mtl_reg_tex(SLOT+2,NULL,NULL);mtl_tex_release(&short_view);
    mtl_reg_tex(SLOT,NULL,NULL);mtl_tex_release(&texture);mtl_queue_forget(SLOT);
    mtl_reg_tex(SLOT+1,NULL,NULL);mtl_tex_release(&ordinary);
    mtl_pool_del(memory);mtl_pool_del(result);free(memory);
    wrapped_buffer_table();
    puts("Wrapped buffer fetches preserve rows, aliases, uniform-handle arrays, nested descriptor helpers and compact nonuniform tables");
}
