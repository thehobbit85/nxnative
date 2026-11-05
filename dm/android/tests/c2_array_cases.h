#ifndef DM_TEST_C2_ARRAY_CASES_H
#define DM_TEST_C2_ARRAY_CASES_H
#include <stdint.h>
#include <string.h>
#define C2_TEST_COUNT 301u
#define C2_TEST_INDICES 304u
#define C2_TEST_COORDS 4u
static const uint32_t c2_test_coords[C2_TEST_COORDS] = {0x3f000000,0x3fc00000,0xbf000000,0x7fc12345};
static uint32_t c2_test_offset(unsigned index) {
    return index < 302 ? index*8u : index == 302 ? 4096u : UINT32_MAX-7u;
}
static void c2_test_inputs(uint64_t textures[256], uint64_t images[32], uint64_t samplers[128]) {
    memset(textures,0,256*sizeof *textures); memset(images,0,32*sizeof *images);
    for(unsigned i=0;i<41;i++) textures[i]=(1u+(i&1))|((uint64_t)(1u+(i&1))<<20);
    for(unsigned i=0;i<8;i++) if(i&1) images[i]=2;
    for(unsigned i=0;i<128;i++) { textures[128+i]=1u+(i&1); samplers[i]=(uint64_t)(1u+(i&1))<<20; }
}
// Literal table ranges, independently of the production driver's C2 lookup.
static uint32_t c2_test_expected(unsigned index,unsigned coord,unsigned separate) {
    if(index>=301) index=300;
    unsigned texture=0,sampler=0;
    if(index>=4 && index<45) {
        unsigned slot=index-4; texture=sampler=1u+(slot&1);
        if(slot>=32 && slot<40 && (slot&1)) { texture=2; sampler=0; }
    } else if(index>=45 && index<173) texture=1u+((index-45)&1);
    else if(index>=173) sampler=1u+((index-173)&1);
    if(separate) sampler|=2;
    if((coord==1 || coord==2) && sampler>=2) return sampler==3 ? 0x3f800000 : 0;
    return texture==1 ? 0x3e800000 : texture==2 ? 0x3f400000 : 0;
}
#endif
