#ifndef TEXTURE_QUERY_SHAPES_CASES_H
#define TEXTURE_QUERY_SHAPES_CASES_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static inline uint32_t tqs_bits(float x) { uint32_t value;memcpy(&value,&x,4);return value; }
static unsigned tqs_layers(unsigned variant) { return variant==1 || variant==3?5:variant==5?6:variant==6?18:1; }
static unsigned tqs_first(unsigned variant) { return variant==1 || variant==3?2:variant==6?6:0; }
static unsigned tqs_visible(unsigned variant) { return variant==1 || variant==3?3:variant==5?6:variant==6?12:1; }
static unsigned tqs_wrap(unsigned index) { return index%2; }
static unsigned tqs_shift(unsigned n,unsigned level) { n>>=level;return n?n:1; }
static void tqs_inputs(unsigned variant,unsigned base,unsigned scale,unsigned level,uint32_t *uniform,uint32_t expected[4]) {
    unsigned mip=base+level,width=8u<<scale,height=4u<<scale,depth=2u<<scale;
    memset(uniform,0,65536);uniform[0]=level;
    expected[0]=tqs_shift(width,mip);
    expected[1]=variant==0?0:variant==1?3:tqs_shift(variant>=5?width:height,mip);
    expected[2]=variant==3?3:variant==4?tqs_shift(depth,mip):variant==6?2:0;
    expected[3]=(variant<2?1:4)-base;
}
static int tqs_check(unsigned variant,unsigned base,unsigned index,const uint32_t *output,const uint32_t expected[4]) {
    for(unsigned word=0;word<256;word++) {
        uint32_t want=word>=64&&word<68?expected[word-64]:0xcccccccc;
        if(output[word]!=want) { fprintf(stderr,"Texture query shapes variant%u base%u case%u word%u: %08x expected %08x\n",variant,base,index,word,output[word],want);return 1; }
    }
    return fwrite(output+64,4,4,stdout)==4?0:1;
}
#endif
