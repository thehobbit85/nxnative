// Match dmsh_build.py's vs_init: an unwritten user output component reads as
// (0,0,0,1) on NVN. The translated body then overwrites its written components.
// The translator emits these as standalone float4 Output variables with Location.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t *spv_output_defaults(const uint32_t *w, size_t nw, size_t *out_n) {
    enum { EntryPoint=15, TypeFloat=22, TypeVector=23, TypePointer=32,
           Constant=43, ConstantComposite=44, Function=54, Variable=59, Store=62,
           Decorate=71, Line=8, NoLine=317, Label=248, Output=3, Location=30 };
    if (nw < 5 || w[0] != 0x07230203 || !w[3]) return NULL;
    uint32_t entry=0, f32=0, v4=0, ptr=0;
    size_t first_fn=0, at=0;
    // Validate instruction bounds before walking the module in subsequent passes.
    for (size_t i=5; i<nw;) {
        uint32_t op=w[i]&0xffff, len=w[i]>>16;
        if (!len || len>nw-i) return NULL;
        if (op==EntryPoint && len>=4 && w[i+1]==0) entry=w[i+2];
        if (op==TypeFloat && len==3 && w[i+2]==32) f32=w[i+1];
        if (op==Function && !first_fn) first_fn=i;
        i+=len;
    }
    if (!entry || !f32 || !first_fn) return NULL;
    for (size_t i=5; i<first_fn; i+=w[i]>>16)
        if ((w[i]&0xffff)==TypeVector && w[i]>>16==4 && w[i+2]==f32 && w[i+3]==4) v4=w[i+1];
    if (!v4) return NULL;
    for (size_t i=5; i<first_fn; i+=w[i]>>16)
        if ((w[i]&0xffff)==TypePointer && w[i]>>16==4 && w[i+2]==Output && w[i+3]==v4) ptr=w[i+1];
    if (!ptr) return NULL;
    uint8_t *located=calloc(w[3], 1);
    if (!located) return NULL;
    for (size_t i=5; i<first_fn; i+=w[i]>>16)
        if ((w[i]&0xffff)==Decorate && w[i]>>16==4 && w[i+2]==Location && w[i+1]<w[3]) located[w[i+1]]=1;
    size_t count=0;
    for (size_t i=5; i<first_fn; i+=w[i]>>16)
        if ((w[i]&0xffff)==Variable && w[i]>>16>=4 && w[i+1]==ptr && w[i+3]==Output && w[i+2]<w[3] && located[w[i+2]]) count++;
    int in_entry=0;
    for (size_t i=first_fn; i<nw; i+=w[i]>>16) {
        uint32_t op=w[i]&0xffff;
        if (op==Function) in_entry=w[i+2]==entry;
        if (op==Label && in_entry) {
            at=i+(w[i]>>16);
            while (at<nw && ((w[at]&0xffff)==Variable || (w[at]&0xffff)==Line || (w[at]&0xffff)==NoLine)) at+=w[at]>>16;
            break;
        }
    }
    if (!count || !at || at>=nw) { free(located); return NULL; }
    uint32_t *o=malloc((nw+15+3*count)*sizeof *o);
    if (!o) { free(located); return NULL; }
    uint32_t bound=w[3], zero=bound++, one=bound++, value=bound++;
    size_t n=first_fn;
    memcpy(o, w, first_fn*sizeof *w);
    const uint32_t decl[]={4u<<16|Constant, f32, zero, 0,
                           4u<<16|Constant, f32, one, 0x3f800000,
                           7u<<16|ConstantComposite, v4, value, zero, zero, zero, one};
    memcpy(o+n, decl, sizeof decl); n+=sizeof decl/sizeof *decl;
    memcpy(o+n, w+first_fn, (at-first_fn)*sizeof *w); n+=at-first_fn;
    for (size_t i=5; i<first_fn; i+=w[i]>>16) {
        if ((w[i]&0xffff)==Variable && w[i]>>16>=4 && w[i+1]==ptr && w[i+3]==Output && w[i+2]<w[3] && located[w[i+2]]) {
            o[n++]=3u<<16|Store; o[n++]=w[i+2]; o[n++]=value;
        }
    }
    memcpy(o+n, w+at, (nw-at)*sizeof *w); n+=nw-at;
    free(located); o[3]=bound; *out_n=n; return o;
}
