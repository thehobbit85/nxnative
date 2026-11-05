#ifndef NVN2_LAYOUT_CASES_H
#define NVN2_LAYOUT_CASES_H
#include <stdint.h>
#include <stdio.h>
enum { NL_WORDS=56, NL_CASES=16 };
static uint32_t nl_word(unsigned bank,unsigned byte,unsigned test) {
    return (0x9e3779b9u*(byte/4+1)) ^ (bank<<24) ^ (test*0x01020304u);
}
static uint32_t nl_expected(unsigned word,unsigned test) {
    unsigned offset=word==55?0x200:word*4,bank=0;
    // Independently fixed intervals for push=19 (rounded32), UBO2, SSBO3,
    // combined2, separate textures3, separate samplers4, images2.
    if(offset>=0x10&&offset<0x30) { bank=24;offset-=0x10; }
    else if(offset>=0x30&&offset<0x50)offset=0x210+offset-0x30;
    else if(offset>=0x50&&offset<0x80)offset=0x310+offset-0x50;
    else if(offset>=0x80&&offset<0x90) { bank=2;offset=0x20+offset-0x80; }
    else if(offset>=0x90&&offset<0xa8) { bank=2;offset=0x168+offset-0x90; }
    else if(offset>=0xa8&&offset<0xc8) { bank=2;offset=0x568+offset-0xa8; }
    else if(offset>=0xc8&&offset<0xd8) { bank=2;offset=0x120+offset-0xc8; }
    return nl_word(bank,offset,test);
}
static int nl_check(unsigned test,const uint32_t *out) {
    for(unsigned i=0;i<256;i++) {
        uint32_t expected=i>=64&&i<64+NL_WORDS?nl_expected(i-64,test):0xcccccccc;
        if(out[i]!=expected) { fprintf(stderr,"NVN2 layout case%u word%u: %08x expected %08x\n",test,i,out[i],expected);return 1; }
    }
    return fwrite(out+64,4,NL_WORDS,stdout)==NL_WORDS?0:1;
}
#endif
