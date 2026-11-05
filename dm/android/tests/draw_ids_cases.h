#ifndef DRAW_IDS_CASES_H
#define DRAW_IDS_CASES_H
#include <stdint.h>
#include <stdio.h>
enum { DI_CASES=32 };
static uint32_t di_base(unsigned test) {
    const uint32_t bases[]={0,7,0x40110010,0x7ffffff0};return bases[test/8];
}
static int32_t di_vertex_base(unsigned test) {
    const int32_t bases[]={-3,0,2,17};return bases[test%4];
}
static unsigned di_first(unsigned test) {
    const unsigned first[]={0,3,53,65535};return first[test%4];
}
static unsigned di_instances(unsigned test) { return 2+test%3; }
static int di_indexed(unsigned test) { return (test%8)>=4; }
static unsigned di_index_offset(unsigned test) { return test%2; }
static unsigned di_vertex(unsigned test) {
    const unsigned indices[]={9,24};return di_indexed(test)?indices[di_index_offset(test)]+di_vertex_base(test):di_first(test);
}
static int di_check(unsigned test,const uint32_t *out) {
    for(unsigned i=0;i<256;i++) {
        uint32_t expected=0xcccccccc;
        if(i>=64 && i<64+4*di_instances(test)) {
            unsigned row=(i-64)/4,col=(i-64)%4;
            expected=col==0?di_vertex(test):col==1?row:col==2?di_base(test):0x44ffe000;
        }
        if(out[i]!=expected) { fprintf(stderr,"Draw IDs case%u word%u: %08x expected %08x\n",test,i,out[i],expected);return 1; }
    }
    return fwrite(out+64,4,16,stdout)==16?0:1;
}
#endif
