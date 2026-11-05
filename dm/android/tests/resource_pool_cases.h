// Pool registration/dummy model, independent of the exported shader lowering.
#ifndef DM_RESOURCE_POOL_CASES_H
#define DM_RESOURCE_POOL_CASES_H
#include <stdint.h>
enum { RP_CONFIGS=5,RP_INDICES=21,RP_COORDS=4,RP_LANES=8 };
static const unsigned rp_texture_bounds[]={1,3,302,4098,65537};
static const unsigned rp_sampler_bounds[]={1,4,129,4095,4095};
static const uint32_t rp_coords[]={0x3f000000,0x3fc00000,0xbf000000,0x7fc12345};
static unsigned rp_texture(unsigned index,unsigned bound) { return index && index<bound && index%11?index%2+1:0; }
static unsigned rp_sampler(unsigned index,unsigned bound) { return index && index<bound && index%7?index%3+1:0; }
static uint32_t rp_handle(unsigned config,unsigned test,unsigned lane) {
    unsigned n=rp_texture_bounds[config],s=rp_sampler_bounds[config];
    const unsigned tids[]={0,1,2,3,31,32,40,127,128,255,256,301,302,4095,4096,65535,65536,0xfffff,n-1,n,n+1};
    const unsigned sids[]={0,1,2,3,31,32,127,128,4095,s-1,s,s==4095?4095:s+1};
    return tids[(test+lane)%RP_INDICES] | (sids[(test*3+lane*7)%12]<<20);
}
static uint32_t rp_expected(unsigned config,unsigned test,unsigned coord,unsigned lane) {
    uint32_t handle=rp_handle(config,test,lane);unsigned tid=handle&0xfffff,sid=handle>>20;
    unsigned texture=rp_texture(tid,rp_texture_bounds[config]),sampler=rp_sampler(sid,rp_sampler_bounds[config]);
    if((coord==1 || coord==2) && sampler>=2)return sampler==3?0x3f800000:0;
    return texture==1?0x3e800000:texture==2?0x3f400000:0;
}
#endif
