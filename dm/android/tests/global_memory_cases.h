// Independent byte-level oracle for the shared raw-address ABI.
#ifndef DM_GLOBAL_MEMORY_CASES_H
#define DM_GLOBAL_MEMORY_CASES_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../gfx/nvn/dmsh_abi.h"
enum { GM_UNIFORM,GM_POINTER,GM_MAP,GM_LOG,GM_OUTPUT,GM_POOL,GM_BUFFERS,GM_VARIANTS=21,GM_ADDRESSES=15 };
static const unsigned gm_sizes[]={65536,256,DMSH_GMEM_MAP_ENTRIES*DMSH_GMEM_MAP_ENTRY,4352,256,1024};
static const unsigned gm_counts[]={0,1,17,4095};
static uint64_t gm_start(unsigned index) { return UINT64_C(0x1fffff000)+(uint64_t)index*0x2000; }
static unsigned gm_hash(uint32_t page) { return (page*UINT32_C(0x9e3779b1))>>22; }
static uint64_t gm_address(unsigned pools,unsigned test) {
    const unsigned offsets[]={0,1,2,3,240,248,252,256};
    if(test<8)return gm_start(0)+offsets[test];
    if(test==8)return gm_start(pools/2);
    if(test==9)return gm_start(pools?pools-1:0);
    if(test==10)return gm_start(pools?pools-1:0)+256;
    if(test==11)return gm_start(0)-4;
    if(test==12)return 0;
    if(test==13)return UINT64_C(0xfffffffffc);
    for(unsigned i=0;i<pools;i++)if(gm_hash((uint32_t)(gm_start(i)>>12))==1023)return gm_start(i);
    return gm_start(0)+1024;
}
static unsigned gm_alignment(unsigned variant) {
    if(variant<2 || variant==7)return 1;
    if(variant<4 || variant==8)return 2;
    return 4;
}
static int gm_pool_offset(uint64_t address,unsigned pools) {
    if(address<gm_start(0))return -1;
    uint64_t relative=address-gm_start(0),index=relative/0x2000,offset=relative%0x2000;
    return index<pools && offset<256?(int)offset:-1;
}
static void gm_prepare(void **buffers,uint64_t device_address,unsigned variant,unsigned pools,unsigned test,unsigned log_mode) {
    for(unsigned i=0;i<GM_BUFFERS;i++)memset(buffers[i],0xcc,gm_sizes[i]);
    memset(buffers[GM_UNIFORM],0,gm_sizes[GM_UNIFORM]);memset(buffers[GM_MAP],0,gm_sizes[GM_MAP]);
    uint64_t *map=buffers[GM_MAP];
    for(unsigned i=1;i<DMSH_GMEM_MAP_ENTRIES;i++)map[i*4]=UINT64_MAX;
    for(unsigned i=0;i<pools;i++) { map[(i+1)*4]=gm_start(i);map[(i+1)*4+1]=gm_start(i)+256;map[(i+1)*4+2]=device_address+256-gm_start(i); }
    uint64_t address=gm_address(pools,test);memcpy(buffers[GM_POINTER],&address,8);
    uint32_t *uniform=buffers[GM_UNIFORM];
    for(unsigned i=0;i<4;i++)uniform[4+i]=(test&1?0xfedcba98u:0x12345678u)+i;
    uint8_t *data=buffers[GM_POOL];for(unsigned i=0;i<256;i++)data[256+i]=(uint8_t)(0x81+i*29);
    // Each log contains 1025 words followed by untouched guard bytes.
    uint32_t *log=buffers[GM_LOG];memset(log,0,4100);
    uint32_t page=(uint32_t)(address>>12);unsigned hash=gm_hash(page);
    if(log_mode==1)log[hash+1]=page;
    if(log_mode==2)for(unsigned i=0;i<8;i++)log[((hash+i)&1023)+1]=0x7f000001u+i;
    (void)variant;
}
static void gm_log_page(uint32_t *log,uint64_t address) {
    uint32_t page=(uint32_t)(address>>12);unsigned hash=gm_hash(page);
    for(unsigned i=0;i<8;i++) {
        unsigned slot=((hash+i)&1023)+1;
        if(!log[slot] || log[slot]==page) { log[slot]=page;return; }
    }
    log[0]=1;
}
static void gm_expected(void **buffers,unsigned variant,unsigned pools,unsigned test,uint8_t *pool,uint32_t *log,uint32_t *output) {
    memcpy(pool,buffers[GM_POOL],1024);memcpy(log,buffers[GM_LOG],4352);memcpy(output,buffers[GM_OUTPUT],256);
    uint64_t address=gm_address(pools,test);uint32_t *values=(uint32_t*)buffers[GM_UNIFORM]+4;
    unsigned words=(variant==5 || variant==10)?2:(variant==6 || variant==11)?4:1;
    for(unsigned word=0;word<words;word++) {
        uint64_t at=address+4*word;int offset=gm_pool_offset(at,pools);uint32_t old=0;
        unsigned width=(variant<2 || variant==7)?1:(variant<4 || variant==8)?2:4;
        if(offset>=0)memcpy(&old,pool+256+offset,width);
        if(variant<7) {
            if(variant==1)old=(uint32_t)(int32_t)(int8_t)old;
            if(variant==3)old=(uint32_t)(int32_t)(int16_t)old;
            output[word]=old;
        } else if(variant<12) {
            if(offset>=0) { memcpy(pool+256+offset,&values[word],width);gm_log_page(log,at); }
        } else {
            output[0]=old;
            if(offset<0)continue;
            uint32_t value=values[0],next=old;
            switch(variant) {
                case 12:next=old+value;break;
                case 13:next=old<value?old:value;break;
                case 14:next=old>value?old:value;break;
                case 15:next=old&value;break;
                case 16:next=old|value;break;
                case 17:next=old^value;break;
                case 18:next=value;break;
                case 19:next=(int32_t)old<(int32_t)value?old:value;break;
                case 20:next=(int32_t)old>(int32_t)value?old:value;break;
            }
            memcpy(pool+256+offset,&next,4);gm_log_page(log,at);
        }
    }
}
static int gm_check(void **buffers,unsigned variant,unsigned pools,unsigned test,unsigned mode,const void *pool,const void *log,const void *output) {
    const void *expected[]={pool,log,output};const unsigned indices[]={GM_POOL,GM_LOG,GM_OUTPUT};
    for(unsigned i=0;i<3;i++)if(memcmp(buffers[indices[i]],expected[i],gm_sizes[indices[i]])) {
        const uint8_t *actual=buffers[indices[i]],*want=expected[i];unsigned byte=0;while(actual[byte]==want[byte])byte++;
        fprintf(stderr,"Global variant%u pools%u address%u log%u buffer%u byte%u: %02x expected %02x\n",variant,pools,test,mode,indices[i],byte,actual[byte],want[byte]);return 1;
    }
    return fwrite(buffers[GM_POOL],1024,1,stdout)!=1 || fwrite(buffers[GM_LOG],4100,1,stdout)!=1 || fwrite(buffers[GM_OUTPUT],16,1,stdout)!=1;
}
#endif
