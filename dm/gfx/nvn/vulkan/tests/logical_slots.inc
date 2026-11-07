static void logical_buffer_slots(void) {
    mtl_queue_activate(1651);
    uint32_t (*pixels)[256]=calloc(32,sizeof *pixels);
    uint32_t uniform[16384]={0},handles[8],output[256];
    tex_t textures[32];
    assert(pixels);
    mtl_pool_add(pixels,32*sizeof *pixels);mtl_pool_add(uniform,sizeof uniform);
    mtl_pool_add(handles,sizeof handles);mtl_pool_add(output,sizeof output);
    for (unsigned slot=0;slot<32;slot++) {
        textures[slot]=(tex_t){.serial=0x17e00+slot,.mem=(uint8_t *)pixels[slot],
            .d={.width=256,.height=1,.depth=1,.levels=1,.format=NVN_FORMAT_R32F,
                .target=slot%5==3 ? NVN_TEXTURE_TARGET_2D : NVN_TEXTURE_TARGET_BUFFER}};
        nvn_tex_layout(&textures[slot].d,&textures[slot].L);
        for (unsigned pixel=0;pixel<256;pixel++) pixels[slot][pixel]=rs_texel(RS_SLOTS,slot,pixel,0);
        mtl_reg_tex(1700+slot,&textures[slot],NULL);
    }
    for (unsigned test=0;test<rs_count(RS_SLOTS);test++) {
        rs_uniform(uniform,RS_SLOTS,0,test);
        uint64_t input=(uintptr_t)handles,result=(uintptr_t)(output+64);
        memcpy(uniform,&input,8);memcpy(uniform+14,&result,8);
        for (unsigned lane=0;lane<8;lane++) handles[lane]=rs_slot(test,lane);
        memset(output,0xcc,sizeof output);
        list_t list={0};
        for (unsigned slot=0;slot<32;slot++) {
            unsigned binding=slot%2 ? NVN_SEPTEX_SLOT0+slot : slot;
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=binding,.h=1700+slot);
        }
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)uniform,.size=sizeof uniform);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&logical_slot_program[0],.stages=0x20);
        RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (unsigned word=0;word<256;word++) {
            uint32_t expected=word>=64 && word<72 ? rs_expected(RS_SLOTS,test,word-64) : 0xcccccccc;
            if (output[word]!=expected) die("logical buffer slots case%u word%u returned %#x expected %#x",test,word,output[word],expected);
        }
    }
    for (unsigned slot=0;slot<32;slot++) { mtl_reg_tex(1700+slot,NULL,NULL);mtl_tex_release(&textures[slot]); }
    mtl_pool_del(pixels);mtl_pool_del(uniform);mtl_pool_del(handles);mtl_pool_del(output);free(pixels);
    mtl_queue_forget(1651);
    puts("Logical buffer slots passed 1792 actual-Maxwell lane reads with separate-slot fallback and mismatched shapes");
}

static void logical_image_slots(void) {
    mtl_queue_activate(1652);
    tex_t textures[32];uint32_t output[32][4];
    mtl_pool_add(output,sizeof output);memset(output,0xcc,sizeof output);
    samp_par white={.ws=NVN_WRAP_MODE_CLAMP_TO_BORDER,.wt=NVN_WRAP_MODE_CLAMP_TO_BORDER,.wr=NVN_WRAP_MODE_CLAMP_TO_BORDER,
        .border={1,1,1,1}},black=white;
    memset(black.border,0,sizeof black.border);
    mtl_reg_samp(1750,&white);mtl_reg_samp(1751,&black);
    list_t list={0};
    p_bind_handles sampler_slots={.stage=5,.first=0,.n=32};
    for (unsigned slot=0;slot<32;slot++) {
        textures[slot]=(tex_t){.serial=0x17f00+slot,.d={.width=1,.height=1,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
        nvn_tex_layout(&textures[slot].d,&textures[slot].L);mtl_reg_tex(1800+slot,&textures[slot],NULL);
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&textures[slot],.r={.width=1,.height=1,.depth=1},.mask=15,.c.f={(slot+1)/32.f});
        unsigned binding=slot%2 ? NVN_SEPTEX_SLOT0+slot : slot;
        uint64_t handle=1800+slot;
        if (!(slot%2)) handle|=1750ull<<20;
        RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=binding,.h=handle);
        if (slot%4!=2) {
            uint64_t sampler=(uint64_t)(slot%4==3 ? 1750 : 1751)<<20;
            sampler_slots.h[slot]=sampler;
        }
        if (slot%16==15) {
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            list=(list_t){0};
        }
    }
    put(&list,OP_BIND_SAMP,&sampler_slots,sizeof sampler_slots);
    RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)output,.size=sizeof output);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&logical_slot_program[1],.stages=0x20);
    RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (unsigned slot=0;slot<32;slot++) {
        uint32_t expected[]={rs_bits((slot+1)/32.f),rs_bits(slot%4>=2 ? 1 : 0),slot%2 ? 0 : slot|(slot<<20),slot%4==2 ? 0 : slot<<20};
        if (memcmp(output[slot],expected,sizeof expected)) die("logical sampled slot %u returned %x %x %x %x expected %x %x %x %x",
            slot,output[slot][0],output[slot][1],output[slot][2],output[slot][3],expected[0],expected[1],expected[2],expected[3]);
        mtl_reg_tex(1800+slot,NULL,NULL);mtl_tex_release(&textures[slot]);
    }
    mtl_pool_del(output);mtl_queue_forget(1652);
    puts("Logical image/sampler slots preserve independent and combined fallback plus logical C2 words");
}
