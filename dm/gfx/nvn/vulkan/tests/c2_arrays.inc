#include "../../../../android/tests/c2_array_cases.h"
static void c2_arrays(void) {
    mtl_queue_activate(1653);
    tex_t textures[2];
    uint64_t texture_slots[256],image_slots[32],sampler_slots[128];
    uint32_t uniform[16384]={0},output[256];
    c2_test_inputs(texture_slots,image_slots,sampler_slots);
    mtl_pool_add(uniform,sizeof uniform);mtl_pool_add(output,sizeof output);
    for (unsigned id=1;id<=3;id++) {
        samp_par sampler={.ws=id>=2 ? NVN_WRAP_MODE_CLAMP_TO_BORDER : NVN_WRAP_MODE_CLAMP_TO_EDGE,
            .wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE};
        if (id==3) for (unsigned c=0;c<4;c++) sampler.border[c]=1;
        mtl_reg_samp(id,&sampler);
    }
    list_t list={0};
    for (unsigned id=1;id<=2;id++) {
        tex_t *texture=&textures[id-1];
        *texture=(tex_t){.serial=0x18000+id,.d={.width=1,.height=1,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
        nvn_tex_layout(&texture->d,&texture->L);mtl_reg_tex(id,texture,NULL);
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=texture,.r={.width=1,.height=1,.depth=1},.mask=15,.c.f={id==1 ? .25f : .75f});
    }
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (unsigned slot=0;slot<256;slot++) {
        list=(list_t){0};
        RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=slot,.h=texture_slots[slot]);
        if (slot<128) { RECORD(OP_BIND_SAMP,p_bind_handles,.stage=5,.first=slot,.n=1,.h={sampler_slots[slot]}); }
        if (slot<32) { RECORD(OP_BIND_IMG,p_bind_handles,.stage=5,.first=slot,.n=1,.h={image_slots[slot]}); }
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    }
    uint64_t address=(uintptr_t)(output+64);memcpy(uniform+14,&address,8);
    unsigned cases=0;
    for (unsigned variant=0;variant<2;variant++) {
        for (unsigned index=0;index<C2_TEST_INDICES;index++) for (unsigned coord=0;coord<C2_TEST_COORDS;coord++) {
            uniform[0]=c2_test_offset(index);uniform[1]=c2_test_coords[coord];memset(output,0xcc,sizeof output);
            list=(list_t){0};
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)uniform,.size=sizeof uniform);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&c2_array_program[variant],.stages=0x20);
            RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (unsigned word=0;word<256;word++) {
                uint32_t expected=word==64 ? c2_test_expected(index,coord,variant) : 0xcccccccc;
                if (output[word]!=expected) die("C2 array variant%u index%u coord%u word%u returned %#x expected %#x",variant,index,coord,word,output[word],expected);
            }
            cases++;
        }
    }
    for (unsigned pass=0;pass<3;pass++) {
        // The same shader's tables shrink to dummy entries, then grow again.
        for (unsigned slot=0;slot<32;slot++) {
            list=(list_t){0};
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=128+slot,.h=pass==1 ? 0 : texture_slots[128+slot]);
            RECORD(OP_BIND_SAMP,p_bind_handles,.stage=5,.first=slot,.n=1,.h={pass==1 ? 0 : sampler_slots[slot]});
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        }
        memset(output,0xcc,sizeof output);list=(list_t){0};
        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)output,.size=sizeof output);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&c2_array_program[2],.stages=0x20);
        RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (unsigned lane=0;lane<32;lane++) {
            uint32_t center=pass==1 ? 0 : lane%2 ? 0x3f400000 : 0x3e800000;
            uint32_t border=pass==1 || lane%2 ? 0 : 0x3f800000;
            if (output[2*lane]!=center || output[2*lane+1]!=border)
                die("separate C2 array pass%u lane%u returned %#x %#x expected %#x %#x",pass,lane,output[2*lane],output[2*lane+1],center,border);
        }
        for (unsigned word=64;word<256;word++) assert(output[word]==0xcccccccc);
    }
    for (unsigned id=1;id<=2;id++) { mtl_reg_tex(id,NULL,NULL);mtl_tex_release(&textures[id-1]); }
    mtl_pool_del(uniform);mtl_pool_del(output);mtl_queue_forget(1653);
    printf("C2 runtime arrays passed %u actual-Maxwell cases plus separate arrays, table resizing, fixed sampler OR and out-of-range indices\n",cases);
}
