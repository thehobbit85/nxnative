// Independent data/address oracle for the actual Maxwell resource fixtures.
#ifndef RESOURCE_SHAPES_CASES_H
#define RESOURCE_SHAPES_CASES_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { RS_WRAPPED, RS_SIZE, RS_SLOTS, RS_MANY, RS_WIDTH=16384, RS_HEIGHT=3 };
static uint32_t rs_bits(float f) { uint32_t u; memcpy(&u,&f,4); return u; }
static unsigned rs_count(unsigned mode) { return mode==RS_MANY?4:mode==RS_SLOTS?224:9; }
static unsigned rs_coord(unsigned mode,unsigned test) {
    const unsigned wrapped[]={0,1,16382,16383,16384,16385,32767,32768,49151},slots[]={0,1,15,31,63,127,255};
    return mode==RS_SLOTS?slots[test%7]:mode==RS_MANY?0:wrapped[test];
}
static uint32_t rs_texel(unsigned mode,unsigned slot,unsigned coord,unsigned test) {
    if(mode==RS_MANY)return rs_bits((float)(slot+1+test*43));
    if(mode==RS_SLOTS)return slot%5==3?0:rs_bits((float)((slot+1)*1000+coord));
    return rs_bits((float)(coord+1));
}
static void rs_uniform(uint32_t *out,unsigned mode,unsigned reg,unsigned test) {
    memset(out,0,65536);
    if(mode==RS_MANY)out[0]=0x1001+reg*71+test*101;
    else { out[0]=0x10000;out[14]=0x20000;out[4]=rs_coord(mode,test); }
}
static uint32_t rs_storage(unsigned reg,unsigned test) { return 0x5001+reg*131+test*17; }
static unsigned rs_slot(unsigned test,unsigned lane) { return (test/7+lane*7)%32; }
static uint32_t rs_expected(unsigned mode,unsigned test,unsigned lane) {
    if(mode==RS_MANY) {
        uint32_t value=0;
        for(unsigned i=0;i<40;i++)value+=rs_texel(mode,i,0,test);
        for(unsigned i=0;i<10;i++)value+=0x1001+i*71+test*101;
        for(unsigned i=0;i<16;i++)value+=rs_storage(i,test);
        return value;
    }
    uint32_t value=rs_texel(mode,rs_slot(test,lane),rs_coord(mode,test),test);
    // The fixture adds the queried width with integer IADD to the fetched bits.
    return value+(mode==RS_SIZE?RS_WIDTH:0);
}
static int rs_check(unsigned mode,unsigned test,const uint32_t *out) {
    unsigned lanes=mode==RS_MANY?1:8;
    for(unsigned word=0;word<256;word++) {
        uint32_t expected=word>=64 && word<64+lanes?rs_expected(mode,test,word-64):0xcccccccc;
        if(out[word]!=expected) { fprintf(stderr,"resource mode%u case%u word%u: %08x expected %08x\n",mode,test,word,out[word],expected);return 1; }
    }
    return fwrite(out+64,4,lanes,stdout)==lanes?0:1;
}
#endif
