#ifndef COMPACT_TEXTURE_CASES_H
#define COMPACT_TEXTURE_CASES_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { CP_CASES=48, CP_VARIANTS=68 };
static uint32_t cp_bits(float x) { uint32_t value;memcpy(&value,&x,4);return value; }
static uint32_t cp_half(float x) { _Float16 h=(_Float16)x;uint16_t value;memcpy(&value,&h,2);return value; }
static unsigned cp_size(unsigned mip) { return 8u>>mip; }
static unsigned cp_count(unsigned variant) { (void)variant;return CP_CASES; }
static unsigned cp_wrap(unsigned index) { return index/4%2; }
static float cp_value(unsigned mip,unsigned x,unsigned y,unsigned component) {
    if(getenv("CP_HALF_EDGES")) {
        // Around underflow, the subnormal/normal transition, ties and overflow.
        // These are finite binary32 values and infinities, with no NaN payloads.
        const uint32_t edge[]={0,0x32ffffff,0x33000000,0x33000001,0x33800000,
            0x33bfffff,0x33c00000,0x33c00001,0x387fdfff,0x387fe000,0x387fe001,
            0x38800000,0x3f800fff,0x3f801000,0x3f801001,0x3f802fff,0x3f803000,
            0x3f803001,0x44811000,0x44813000,0x477fe000,0x477fefff,0x477ff000,
            0x477ff001,0x47800000,0x7f7fffff,0x7f800000};
        unsigned index=mip*64+y*8+x+component*11;
        uint32_t bits=edge[index%(sizeof edge/sizeof edge[0])]|((index&1)<<31);
        float value;memcpy(&value,&bits,4);return value;
    }
    const float bases[]={-1,100,-300,1000};float delta=(float)(mip*32+y*4)+(float)x*.25f;
    return bases[component]+(component%2?delta:-delta);
}
static void cp_pixels(float *pixels,unsigned mip) {
    unsigned size=cp_size(mip);for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++)for(unsigned c=0;c<4;c++)pixels[(y*size+x)*4+c]=cp_value(mip,x,y,c);
}
static int cp_address(int x,int size,unsigned repeat) { return repeat?((x%size)+size)%size:x<0?0:x>=size?size-1:x; }
static void cp_inputs(unsigned variant,unsigned index,uint32_t *uniform,uint32_t expected[8]) {
    unsigned op=variant<52?variant/26:2,half=variant<52?variant/13%2:(variant-52)/8,dual=variant<52?variant%13>=8:(variant-52)/4%2;
    unsigned mask=variant<52?(dual?variant%13-8:variant%13):4,component=variant<52?0:(variant-52)%4;
    const unsigned masks[2][8]={{1,2,4,8,3,9,10,12},{7,11,13,14,15,0,0,0}};
    const float positions[][2]={{1.f/16,5.f/16},{5.f/16,9.f/16},{9.f/16,13.f/16},{13.f/16,1.f/16},{-.125f,1.125f},{1.125f,-.125f}};
    unsigned mip=op==2?0:index%4,size=cp_size(mip),point=index/8,repeat=cp_wrap(index);float u=positions[point][0],v=positions[point][1];
    memset(uniform,0,65536);uniform[0]=op==1?point%size:cp_bits(u);uniform[1]=op==1?(point*3+1)%size:cp_bits(v);uniform[2]=op==1?mip:cp_bits((float)mip);
    float values[4];
    if(op==2) {
        int x=(int)floorf(u*size-.5f),y=(int)floorf(v*size-.5f);const int dx[]={0,1,1,0},dy[]={1,1,0,0};
        for(unsigned c=0;c<4;c++)values[c]=cp_value(0,(unsigned)cp_address(x+dx[c],(int)size,repeat),(unsigned)cp_address(y+dy[c],(int)size,repeat),component);
    } else {
        unsigned x=op==1?uniform[0]:(unsigned)cp_address((int)floorf(u*size),(int)size,repeat),y=op==1?uniform[1]:(unsigned)cp_address((int)floorf(v*size),(int)size,repeat);
        for(unsigned c=0;c<4;c++)values[c]=cp_value(mip,x,y,c);
    }
    for(unsigned i=0;i<8;i++)expected[i]=0xc0de0010+i;
    float selected[4]={0};unsigned n=0;for(unsigned c=0;c<4;c++)if(masks[dual][mask]&(1u<<c))selected[n++]=values[c];
    if(half) { expected[0]=cp_half(selected[0])|(cp_half(selected[1])<<16);if(dual)expected[4]=cp_half(selected[2])|(cp_half(selected[3])<<16); }
    else for(unsigned i=0;i<n;i++)expected[(i/2)*4+i%2]=cp_bits(selected[i]);
}
static int cp_check(unsigned variant,unsigned index,const uint32_t *output,const uint32_t expected[8]) {
    for(unsigned word=0;word<256;word++) {
        uint32_t want=word>=64&&word<72?expected[word-64]:0xcccccccc;
        if(output[word]!=want) { fprintf(stderr,"Compact texture variant%u case%u word%u: %08x expected %08x\n",variant,index,word,output[word],want);return 1; }
    }
    return fwrite(output+64,4,8,stdout)==8?0:1;
}
#endif
