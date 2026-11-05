// The oracle distinguishes bank, byte address, access width and sign extension.
#ifndef CONSTANT_BANKS_CASES_H
#define CONSTANT_BANKS_CASES_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { CB_VARIANTS=36, CB_CASES=40 };
static unsigned cb_bank(unsigned variant) { const unsigned banks[]={0,1,2,4,16,24};return banks[variant/6]; }
static unsigned cb_bytes(unsigned width) { return width<2?1:width<4?2:width==4?4:8; }
static unsigned cb_limit(unsigned bank) { return bank==1||bank==24?256:bank==0||bank==2?4096:65536; }
static uint8_t cb_pattern(unsigned bank,unsigned index) { return (uint8_t)(index*73+(index>>8)*29+bank*37+19); }
static unsigned cb_offset(unsigned bank,unsigned width,unsigned test) {
    unsigned limit=cb_limit(bank),bytes=cb_bytes(width);
    unsigned offset=test<32?test:test==32?63:test==33?127:test==34?239:test==35?240:test==36?247:test==37?248:test==38?limit/2:limit-8;
    return offset&~(bytes-1);
}
static int cb_decode(const char *hex,uint8_t *out,size_t size) {
    if(strlen(hex)!=2*size)return 1;
    for(size_t i=0;i<size;i++) {
        unsigned value=0;
        for(unsigned j=0;j<2;j++) { char c=hex[2*i+j];if(c>='0'&&c<='9')value=value*16+(unsigned)(c-'0');else if(c>='a'&&c<='f')value=value*16+(unsigned)(c-'a'+10);else return 1; }
        out[i]=(uint8_t)value;
    }
    return 0;
}
static int cb_check(unsigned variant,unsigned test,const uint32_t *out) {
    unsigned bank=cb_bank(variant),width=variant%6,offset=cb_offset(bank,width,test),bytes=cb_bytes(width);
    uint32_t expected[2]={0,0x76543210};uint8_t input[8];
    for(unsigned i=0;i<8;i++)input[i]=cb_pattern(bank,offset+i);
    memcpy(expected,input,bytes);
    if(width==1)expected[0]=(uint32_t)(int32_t)(int8_t)input[0];
    if(width==3) { int16_t v;memcpy(&v,input,2);expected[0]=(uint32_t)(int32_t)v; }
    for(unsigned word=0;word<256;word++) {
        uint32_t want=word==64?expected[0]:word==65?expected[1]:0xcccccccc;
        if(out[word]!=want) { fprintf(stderr,"bank%u width%u offset%u word%u: %08x expected %08x\n",bank,width,offset,word,out[word],want);return 1; }
    }
    return fwrite(out+64,4,2,stdout)==2?0:1;
}
#endif
