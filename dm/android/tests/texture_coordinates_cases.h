#ifndef TEXTURE_COORDINATES_CASES_H
#define TEXTURE_COORDINATES_CASES_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t tco_bits(float x) { uint32_t value;memcpy(&value,&x,4);return value; }
static unsigned tco_size(unsigned mip) { return 8u>>mip; }
static unsigned tco_layers(unsigned variant) { return variant>=2?(variant%2?18:6):(variant%2?4:1); }
static unsigned tco_count(unsigned variant) { return variant>=2?6*4*4*4*(variant%2?2:1):8*4*2*(variant%2?3:1); }
static unsigned tco_wrap(unsigned index) { return index/32%2; }
static float tco_value(unsigned mip,unsigned layer,unsigned x,unsigned y,unsigned component) { return (float)(1000000*component+100000*layer+10000*mip+100*y+x+1); }
static void tco_pixels(float *pixels,unsigned variant,unsigned mip,unsigned layer) {
    unsigned size=tco_size(mip),height=variant>=2?size:1;
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<size;x++)for(unsigned c=0;c<4;c++)pixels[(y*size+x)*4+c]=tco_value(mip,layer,x,y,c);
}
static int tco_address(int x,int size,unsigned repeat) { return repeat?((x%size)+size)%size:x<0?0:x>=size?size-1:x; }
static void tco_inputs(unsigned variant,unsigned base,unsigned index,uint32_t *uniform,uint32_t expected[4]) {
    unsigned cube=variant>=2,array=variant%2,level=index%4,mip=base+level;if(mip>3)mip=3;
    unsigned size=tco_size(mip),physical=0;int x=0,y=0;memset(uniform,0,65536);uniform[4]=tco_bits((float)level);
    if(cube) {
        unsigned face=index/4%6,pu=index/24%4,pv=index/96%4,layer=array?index/384%2:0;
        // Keep the normal projection corpus away from texel boundaries.
        // The optional exact-boundary probe retains native rounding differences.
        float inset=getenv("TCO_BOUNDARY_COORDS")?0:.25f;
        float u=(2*pu+1+inset)/8,v=(2*pv+1+inset)/8,s=2*u-1,t=2*v-1;
        const float directions[][3]={{1,-t,-s},{-1,-t,s},{s,1,t},{s,-1,-t},{s,-t,1},{-s,-t,-1}};
        unsigned start=array?1:0;if(array)uniform[0]=layer;
        // Vary vector magnitude without changing the selected face or texel.
        float scale=(index/4%2)?2:.5f;
        for(unsigned c=0;c<3;c++)uniform[start+c]=tco_bits(directions[face][c]*scale);
        physical=(array?layer+1:0)*6+face;x=(int)floorf(u*size);y=(int)floorf(v*size);
    } else {
        const float positions[]={-.25f,-.0625f,1.f/16,3.f/16,7.f/16,11.f/16,15.f/16,1.25f};
        float u=positions[index/4%8];unsigned layer=array?index/64%3:0;
        uniform[0]=array?layer:tco_bits(u);if(array)uniform[1]=tco_bits(u);
        physical=array?layer+1:0;x=tco_address((int)floorf(u*size),(int)size,tco_wrap(index));
    }
    for(unsigned c=0;c<4;c++)expected[c]=tco_bits(tco_value(mip,physical,(unsigned)x,(unsigned)y,c));
}
static int tco_check(unsigned variant,unsigned base,unsigned index,const uint32_t *output,const uint32_t expected[4]) {
    for(unsigned word=0;word<256;word++) {
        uint32_t want=word>=64&&word<68?expected[word-64]:0xcccccccc;
        if(output[word]!=want) { fprintf(stderr,"Texture coordinates variant%u base%u case%u word%u: %08x expected %08x\n",variant,base,index,word,output[word],want);return 1; }
    }
    return fwrite(output+64,4,4,stdout)==4?0:1;
}
#endif
