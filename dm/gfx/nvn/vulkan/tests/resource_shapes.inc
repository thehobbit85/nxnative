// Optional actual-Maxwell replay: the export has 66 independently live bindings.
#include "../../../../android/tests/resource_shapes_cases.h"
static void many_resource_bindings(void) {
    if (!getenv("DM_VK_RESOURCE_SHAPES")) return;
    uint32_t (*uniform)[16384] = calloc(10, sizeof *uniform);
    _Alignas(256) uint32_t storage[16][256]; tex_t textures[40];
    assert(uniform); mtl_pool_add(uniform,10*sizeof *uniform);mtl_pool_add(storage,sizeof storage);
    samp_par sampler={0};sampler.ws=sampler.wt=sampler.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE;
    mtl_reg_samp(1250,&sampler);
    for(unsigned i=0;i<40;i++) {
        textures[i]=(tex_t){.serial=0xf100+i,.d={.width=1,.height=1,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
        nvn_tex_layout(&textures[i].d,&textures[i].L);mtl_reg_tex(1200+(int)i,&textures[i],NULL);
    }
    for(unsigned test=0;test<4;test++) {
        list_t list={0};
        for(unsigned i=0;i<40;i++) {
            p_clear_tex clear={.tex=&textures[i],.r={.width=1,.height=1,.depth=1},.mask=15};
            uint32_t value=rs_texel(RS_MANY,i,0,test);memcpy(&clear.c.f[0],&value,4);put(&list,OP_CLEAR_TEX,&clear,sizeof clear);
        }
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));list.length=0;
        for(unsigned i=0;i<10;i++) {
            rs_uniform(uniform[i],RS_MANY,i,test);
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=i,.addr=(uintptr_t)uniform[i],.size=sizeof uniform[i]);
        }
        memset(storage,0xcc,sizeof storage);
        for(unsigned i=0;i<16;i++) {
            storage[i][64]=rs_storage(i,test);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=i,.addr=(uintptr_t)&storage[i][64],.size=4);
        }
        for(unsigned i=0;i<40;i++)RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=i,.h=1200+i);
        RECORD(OP_BIND_SAMP,p_bind_handles,.stage=5,.first=1,.n=1,.h={1250ull<<20});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&resource_shapes_program,.stages=0x20);
        RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(unsigned i=0;i<16;i++)for(unsigned word=0;word<256;word++) {
            uint32_t expected=word==64?(i==15?rs_expected(RS_MANY,test,0):rs_storage(i,test)):0xcccccccc;
            if(storage[i][word]!=expected) { fprintf(stderr,"many bindings case%u storage%u word%u: %08x expected %08x\n",test,i,word,storage[i][word],expected);abort(); }
        }
    }
    for(unsigned i=0;i<40;i++) { mtl_reg_tex(1200+(int)i,NULL,NULL);mtl_tex_release(&textures[i]); }
    mtl_pool_del(storage);mtl_pool_del(uniform);free(uniform);
    puts("Actual Maxwell compute with 66 bindings passed four distinct resource configurations");
}
