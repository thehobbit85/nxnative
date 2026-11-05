#ifndef TEXTURE_COMPARE_CASES_H
#define TEXTURE_COMPARE_CASES_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { TC_CASES=504 };
static uint32_t tc_bits(float x) { uint32_t value;memcpy(&value,&x,4);return value; }
static unsigned tc_size(unsigned mip) { return 8u>>mip; }
static unsigned tc_wrap(unsigned index) { return index/4%2; }
static float tc_depth(unsigned mip,unsigned layer,unsigned x,unsigned y) { return (float)((17*x+11*y+5*mip+7*layer)%64)/64; }
static inline void tc_pixels(float *pixels,unsigned mip,unsigned layer) {
    unsigned size=tc_size(mip);for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++)pixels[y*size+x]=tc_depth(mip,layer,x,y);
}
static int tc_address(int coordinate,int size,unsigned repeat) { return repeat?((coordinate%size)+size)%size:coordinate<0?0:coordinate>=size?size-1:coordinate; }
static float tc_test(unsigned mip,unsigned layer,int x,int y,unsigned repeat,float reference) {
    int size=(int)tc_size(mip);return reference<=tc_depth(mip,layer,(unsigned)tc_address(x,size,repeat),(unsigned)tc_address(y,size,repeat))?1:0;
}
static void tc_inputs(unsigned variant,unsigned linear,unsigned index,uint32_t *uniform,uint32_t expected[4]) {
    const float references[]={-.125f,0,.25f,.5f,.75f,1,1.125f};
    const float positions[][2]={{11.f/64,29.f/64},{-.125f,1.125f},{51.f/64,11.f/64}};
    unsigned gather=variant%2,array=variant/2,mip=gather?0:index%4,point=index/56%3,layer=array?index/8%3:0,repeat=tc_wrap(index);
    float u=positions[point][0],v=positions[point][1],reference=references[index/8%7];
    int offset=(int)(index/168)-1,ox=offset,oy=-offset;unsigned size=tc_size(mip);
    memset(uniform,0,65536);uniform[0]=array?layer:tc_bits(u);uniform[1]=tc_bits(array?u:v);uniform[2]=array?tc_bits(v):0;uniform[3]=tc_bits((float)(index%4));
    uniform[4]=((unsigned)ox&(gather?63:15))|(((unsigned)oy&(gather?63:15))<<(gather?8:4));uniform[5]=tc_bits(reference);
    unsigned physical=array?layer+1:0;
    if(gather) {
        int x=(int)floorf(u*size-.5f)+ox,y=(int)floorf(v*size-.5f)+oy;const int dx[]={0,1,1,0},dy[]={1,1,0,0};
        for(unsigned c=0;c<4;c++)expected[c]=tc_bits(tc_test(mip,physical,x+dx[c],y+dy[c],repeat,reference));
    } else {
        float answer;
        if(linear) {
            float x=u*size+ox-.5f,y=v*size+oy-.5f;int ix=(int)floorf(x),iy=(int)floorf(y);float fx=x-ix,fy=y-iy;
            answer=(tc_test(mip,physical,ix,iy,repeat,reference)*(1-fx)+tc_test(mip,physical,ix+1,iy,repeat,reference)*fx)*(1-fy)+
                   (tc_test(mip,physical,ix,iy+1,repeat,reference)*(1-fx)+tc_test(mip,physical,ix+1,iy+1,repeat,reference)*fx)*fy;
        } else answer=tc_test(mip,physical,(int)floorf(u*size)+ox,(int)floorf(v*size)+oy,repeat,reference);
        expected[0]=tc_bits(answer);expected[1]=0x76543210;expected[2]=0x12345678;expected[3]=0xabcdef01;
    }
}
static int tc_matches(unsigned filtered,uint32_t actual,uint32_t expected) {
    if(actual==expected)return 1;
    // Native depth filtering is implementation-dependent (Vulkan Sampling,
    // Depth Compare Operation). This is a strict corpus comparison bound,
    // not a precision guarantee from the API. Keep 0/1 and guards exact.
    if(!filtered || expected==tc_bits(0) || expected==tc_bits(1))return 0;
    float got,want;memcpy(&got,&actual,4);memcpy(&want,&expected,4);
    return isfinite(got) && got>=0 && got<=1 && fabsf(got-want)<=0x1p-12f;
}
static int tc_check(unsigned variant,unsigned linear,unsigned index,const uint32_t *output,const uint32_t expected[4]) {
    for(unsigned word=0;word<256;word++) {
        uint32_t want=word>=64&&word<68?expected[word-64]:0xcccccccc;
        if(!tc_matches(linear && !(variant%2) && word==64,output[word],want)) { fprintf(stderr,"Depth compare variant%u linear%u case%u word%u: %08x expected %08x\n",variant,linear,index,word,output[word],want);return 1; }
    }
    return fwrite(output+64,4,4,stdout)==4?0:1;
}
#endif
