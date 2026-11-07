static void resource_pools(void) {
    mtl_queue_activate(1654);
    const unsigned texture_ids[]={1,2,31,32,301,4096,65536};
    const unsigned sampler_ids[]={1,2,31,32,127,4095};
    const unsigned queries[]={0,1,2,3,31,32,127,128,301,302,4095,4096,65535,65536,65537,0xfffff};
    tex_t textures[2];uint32_t uniform[16384]={0},handles[8],output[256];
    mtl_pool_add(uniform,sizeof uniform);mtl_pool_add(handles,sizeof handles);mtl_pool_add(output,sizeof output);
    uint64_t input=(uintptr_t)handles,result=(uintptr_t)(output+64);memcpy(uniform,&input,8);memcpy(uniform+14,&result,8);
    list_t list={0};
    for (unsigned i=0;i<2;i++) {
        textures[i]=(tex_t){.serial=0x18100+i,.d={.width=1,.height=1,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
        nvn_tex_layout(&textures[i].d,&textures[i].L);
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&textures[i],.r={.width=1,.height=1,.depth=1},.mask=15,.c.f={i ? .75f : .25f});
    }
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    unsigned cases=0;
    for (unsigned pass=0;pass<4;pass++) {
        int alternate=pass>=2,empty=pass==1;
        int texture_base=alternate ? 4096 : 2048,sampler_base=alternate ? 512 : 256;
        for (unsigned i=0;i<sizeof texture_ids/sizeof *texture_ids;i++) {
            unsigned texel=(texture_ids[i]+alternate)&1;
            mtl_reg_tex(texture_base+texture_ids[i],empty ? NULL : &textures[texel],NULL);
        }
        for (unsigned i=0;i<sizeof sampler_ids/sizeof *sampler_ids;i++) {
            unsigned white=(sampler_ids[i]+pass)&1;
            samp_par sampler={.ws=NVN_WRAP_MODE_CLAMP_TO_BORDER,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE};
            for (unsigned c=0;c<4;c++) sampler.border[c]=(float)white;
            mtl_reg_samp(sampler_base+sampler_ids[i],&sampler);
        }
        for (unsigned test=0;test<16;test++) for (unsigned outside=0;outside<2;outside++) {
            uniform[4]=outside ? 0x3fc00000 : 0x3f000000;memset(output,0xcc,sizeof output);
            for (unsigned lane=0;lane<8;lane++) {
                unsigned tid=queries[(test+lane)%16],sid=queries[(3*test+lane)%16]&4095;
                handles[lane]=tid|(sid<<20);
            }
            list=(list_t){0};
            RECORD(OP_SET_TEX_POOL,p_set_pool,.pool=alternate ? &alternate_texture_pool : &texture_pool);
            RECORD(OP_SET_SAMP_POOL,p_set_pool,.pool=alternate ? &alternate_sampler_pool : &sampler_pool);
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)uniform,.size=sizeof uniform);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&resource_pool_program,.stages=0x20);
            RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (unsigned word=0;word<256;word++) {
                uint32_t expected=0xcccccccc;
                if (word>=64 && word<72) {
                    unsigned tid=handles[word-64]&0xfffff,sid=handles[word-64]>>20,real_texture=0,real_sampler=0;
                    for (unsigned i=0;i<sizeof texture_ids/sizeof *texture_ids;i++) real_texture|=tid==texture_ids[i];
                    for (unsigned i=0;i<sizeof sampler_ids/sizeof *sampler_ids;i++) real_sampler|=sid==sampler_ids[i];
                    expected=real_texture && !empty ? ((tid+alternate)&1 ? 0x3f400000 : 0x3e800000) : 0;
                    if (outside && real_sampler) expected=(sid+pass)&1 ? 0x3f800000 : 0;
                }
                if (output[word]!=expected) die("resource pool pass%u case%u outside%u word%u returned %#x expected %#x",pass,test,outside,word,output[word],expected);
            }
            cases+=8;
        }
    }
    for (unsigned pool=0;pool<2;pool++) for (unsigned i=0;i<sizeof texture_ids/sizeof *texture_ids;i++)
        mtl_reg_tex((pool ? 4096 : 2048)+texture_ids[i],NULL,NULL);
    for (unsigned i=0;i<2;i++) mtl_tex_release(&textures[i]);
    mtl_pool_del(uniform);mtl_pool_del(handles);mtl_pool_del(output);mtl_queue_forget(1654);
    printf("Resource pools passed %u actual-Maxwell lane samples across sparse IDs, pool changes, empty entries and re-registration\n",cases);
}
