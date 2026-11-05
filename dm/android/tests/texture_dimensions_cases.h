#ifndef TEXTURE_DIMENSIONS_CASES_H
#define TEXTURE_DIMENSIONS_CASES_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t td_bits(float x) { uint32_t value;memcpy(&value,&x,4);return value; }
static unsigned td_size(unsigned mip) { unsigned size=8u>>mip;return size?size:1; }
static unsigned td_count(unsigned volume) { return (volume?4096:256)+192; }
static unsigned td_wrap(unsigned index) { return (index/4)%2; }
static float td_value(unsigned mip,unsigned layer,unsigned x,unsigned y,unsigned z,unsigned component) {
    return (float)(1000000*component+100000*layer+10000*mip+1000*z+100*y+x+1);
}
static void td_pixels(float *pixels,unsigned volume,unsigned mip,unsigned layer) {
    unsigned size=td_size(mip),depth=volume?size:1;
    for(unsigned z=0;z<depth;z++)for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++)for(unsigned c=0;c<4;c++)
        pixels[((z*size+y)*size+x)*4+c]=td_value(mip,layer,x,y,z,c);
}
static int td_address(int coordinate,int size,unsigned repeat) {
    return repeat?((coordinate%size)+size)%size:coordinate<0?0:coordinate>=size?size-1:coordinate;
}
static void td_inputs(unsigned volume,unsigned base,unsigned index,uint32_t *uniform,uint32_t expected[4]) {
    // Sweep every offset at a spatially distinguishing mip. Cycling LOD with
    // the tuple index would hide whole offset families at the 1x1x1 mip.
    unsigned cube=volume?4096:256,code=index%cube,level=index<cube?0:index%4,point=index<cube?1:(index-cube)/8%4,layer=index/8%3;
    int offsets[]={ (int)(code%16)-8,(int)(code/16%16)-8,volume?(int)(code/256%16)-8:0 };
    if(index>=cube) { offsets[0]=(int)((index-cube)/32%3)-1;offsets[1]=-offsets[0];offsets[2]=volume?offsets[0]:0; }
    const float coords[]={-.125f,11.f/64,51.f/64,1.125f};float u=coords[point],v=coords[(point+1)%4],w=coords[(point+2)%4];
    memset(uniform,0,65536);
    uniform[0]=volume?td_bits(u):layer;uniform[1]=td_bits(volume?v:u);uniform[2]=td_bits(volume?w:v);uniform[3]=td_bits((float)level);
    uniform[4]=((unsigned)offsets[0]&15)|(((unsigned)offsets[1]&15)<<4)|(((unsigned)offsets[2]&15)<<8);
    unsigned mip=base+level;if(mip>3)mip=3;int size=(int)td_size(mip);unsigned repeat=td_wrap(index);
    unsigned x=(unsigned)td_address((int)floorf(u*size)+offsets[0],size,repeat),y=(unsigned)td_address((int)floorf(v*size)+offsets[1],size,repeat);
    unsigned z=volume?(unsigned)td_address((int)floorf(w*size)+offsets[2],size,repeat):0;
    for(unsigned c=0;c<4;c++)expected[c]=td_bits(td_value(mip,volume?0:layer+1,x,y,z,c));
}
static int td_check(unsigned volume,unsigned base,unsigned index,const uint32_t *output,const uint32_t expected[4]) {
    for(unsigned word=0;word<256;word++) {
        uint32_t want=word>=64&&word<68?expected[word-64]:0xcccccccc;
        if(output[word]!=want) { fprintf(stderr,"Texture dimensions volume%u base%u case%u word%u: %08x expected %08x\n",volume,base,index,word,output[word],want);return 1; }
    }
    return fwrite(output+64,4,4,stdout)==4?0:1;
}
#endif
