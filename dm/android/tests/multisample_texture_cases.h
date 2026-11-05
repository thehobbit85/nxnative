#ifndef MULTISAMPLE_TEXTURE_CASES_H
#define MULTISAMPLE_TEXTURE_CASES_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { MS_SIDE=4, MS_SAMPLES=4 };
static uint32_t ms_bits(float x) { uint32_t value;memcpy(&value,&x,4);return value; }
static unsigned ms_count(unsigned variant) { return variant%2?1:MS_SIDE*MS_SIDE*MS_SAMPLES*(variant>=2?2:1); }
static void ms_inputs(unsigned variant,unsigned index,uint32_t *uniform,uint32_t expected[4]) {
    unsigned array=variant>=2,sample=index%MS_SAMPLES,x=index/MS_SAMPLES%MS_SIDE,y=index/(MS_SAMPLES*MS_SIDE)%MS_SIDE,layer=index/(MS_SAMPLES*MS_SIDE*MS_SIDE);
    memset(uniform,0,65536);uniform[0]=array?layer:x;uniform[1]=array?x:y;uniform[2]=array?y:0;uniform[3]=sample;
    if(variant%2) { expected[0]=MS_SAMPLES;expected[1]=0x76543210;expected[2]=0x12345678;expected[3]=0xabcdef01; }
    else for(unsigned c=0;c<4;c++)expected[c]=ms_bits((float)(100000*c+10000*(array?layer+1:0)+1000*sample+100*y+x+1));
}
static int ms_check(unsigned variant,unsigned index,const uint32_t *output,const uint32_t expected[4]) {
    for(unsigned word=0;word<256;word++) {
        uint32_t want=word>=64&&word<68?expected[word-64]:0xcccccccc;
        if(output[word]!=want) { fprintf(stderr,"Multisample variant%u case%u word%u: %08x expected %08x\n",variant,index,word,output[word],want);return 1; }
    }
    return fwrite(output+64,4,4,stdout)==4?0:1;
}
#endif
