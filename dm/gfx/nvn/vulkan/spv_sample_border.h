// Metal's two-stock-color identity, applied to ordinary float samples. The
// exported shaders inline image helpers before reaching this runtime pass.
#pragma once
#include "spv_wrapped_buffer.h"
typedef struct { uint32_t binding,twin_binding,nearest,other; float factor[4]; } sample_border;
typedef struct { uint32_t root,clone; } border_id;
#define BORDER_OP(dst, op, ...) do { \
    uint32_t code[]={0,__VA_ARGS__};code[0]=(uint32_t)(sizeof code/4)<<16|(op); \
    if (!texel_append(dst,code,sizeof code/4)) goto fail; \
} while (0)
static uint32_t *spv_sample_border(const uint32_t *words,size_t count,const sample_border *edits,size_t nedits,
                                  size_t *out_count,const char **reason) {
    *out_count=0;*reason="invalid custom-border module";
    if (count<5 || words[0]!=0x07230203 || words[1]>0x10300 || !words[3] || words[3]>0x3fffff) return NULL;
    wrapped_module m={.words=words,.bound=words[3],.module={.next=words[3]}};
    m.ids=calloc(m.bound,sizeof *m.ids);
    border_id *ids=calloc(m.bound,sizeof *ids);
    uint32_t *factors=calloc(nedits,sizeof *factors);
    texel_words annotations={0},out={0};size_t functions=count,found=0;
    if (!m.ids || !ids || !factors) goto fail;
    for (size_t i=5;i<count;) {
        const uint32_t *p=words+i;unsigned len=p[0]>>16,op=p[0]&0xffff;
        if (!len || len>count-i) goto fail;
        if (op>=19 && op<=33 && len>=2 && p[1]<m.bound) m.ids[p[1]].declaration=i;
        else if (len>=3 && p[1]<m.bound && m.ids[p[1]].declaration && p[2]<m.bound) m.ids[p[2]].type=p[1];
        if (op==71 && len==4 && p[1]<m.bound && p[2]==33) for (size_t e=0;e<nedits;e++)
            if (p[3]==edits[e].binding) ids[p[1]].root=(uint32_t)e+1;
        if (op==59 && len==4 && p[2]<m.bound && ids[p[2]].root) {
            if (p[3]!=0) goto fail;
            ids[p[2]].clone=m.module.next++;found++;
        }
        if (op==54 && functions==count) functions=i;
        i+=len;
    }
    if (found!=nedits || functions==count) goto fail;
    for (size_t i=5;i<functions;i+=words[i]>>16) {
        const uint32_t *p=words+i;unsigned len=p[0]>>16,op=p[0]&0xffff;
        if (op==59 && len==4 && ids[p[2]].root) {
            const sample_border *edit=&edits[ids[p[2]].root-1];
            BORDER_OP(&m.module.globals,59,p[1],ids[p[2]].clone,p[3]);
            BORDER_OP(&annotations,71,ids[p[2]].clone,34,0);
            BORDER_OP(&annotations,71,ids[p[2]].clone,33,edit->twin_binding);
        }
    }
    for (size_t i=functions;i<count;i+=words[i]>>16) {
        const uint32_t *p=words+i;unsigned len=p[0]>>16,op=p[0]&0xffff;
        unsigned source=op==86 ? 4 : 3;
        uint32_t root=len>source && p[source]<m.bound ? ids[p[source]].root : 0;
        if (root && (op==61 || op==83 || op==86 || op==100)) {
            if (len>8 || p[2]>=m.bound || !ids[p[source]].clone) goto fail;
            ids[p[2]]=(border_id){root,m.module.next++};
            uint32_t clone[8];memcpy(clone,p,len*4);clone[2]=ids[p[2]].clone;clone[source]=ids[p[source]].clone;
            if (!texel_append(&m.module.body,p,len) || !texel_append(&m.module.body,clone,len)) goto fail;
            continue;
        }
        if (root && (op==87 || op==88 || op==91 || op==92)) {
            if (len>16 || p[1]>=m.bound || !m.ids[p[1]].declaration || !ids[p[3]].clone) goto fail;
            const uint32_t *vector=words+m.ids[p[1]].declaration;
            if (vector[0]!=(4u<<16|23) || vector[3]!=4 || vector[2]>=m.bound || !m.ids[vector[2]].declaration) goto fail;
            const uint32_t *scalar=words+m.ids[vector[2]].declaration;
            if (scalar[0]!=(3u<<16|22) || scalar[2]!=32) goto fail;
            uint32_t *factor=&factors[root-1];
            if (!*factor) {
                uint32_t components[4];
                for (unsigned ch=0;ch<4;ch++) {
                    uint32_t bits;memcpy(&bits,&edits[root-1].factor[ch],4);
                    components[ch]=texel_constant(&m.module,vector[2],bits);if (!components[ch]) goto fail;
                }
                *factor=m.module.next++;
                BORDER_OP(&m.module.globals,44,p[1],*factor,components[0],components[1],components[2],components[3]);
            }
            uint32_t ordinary[16],twin[16];memcpy(ordinary,p,len*4);memcpy(twin,p,len*4);
            ordinary[2]=m.module.next++;twin[2]=m.module.next++;twin[3]=ids[p[3]].clone;
            if (!texel_append(&m.module.body,ordinary,len) || !texel_append(&m.module.body,twin,len)) goto fail;
            uint32_t difference=m.module.next++,correction=m.module.next++;
            BORDER_OP(&m.module.body,131,p[1],difference,twin[2],ordinary[2]);
            BORDER_OP(&m.module.body,133,p[1],correction,difference,*factor);
            BORDER_OP(&m.module.body,129,p[1],p[2],ordinary[2],correction);
            continue;
        }
        // Descriptor arrays and non-inlined helper calls must not silently
        // lose their correction. This first pass selects scalar bindings only.
        unsigned from=len,end=len;
        if (op==57) from=4;
        if (op==65 || op==66 || op==67 || op==70) { from=3;end=4; }
        if (op==62 || op==63 || op==64) { from=1;end=3; }
        if (op==254) from=1;
        if (end>len) goto fail;
        for (unsigned arg=from;arg<end;arg++) if (p[arg]<m.bound && ids[p[arg]].root) {
            *reason="custom-border sampler requires scalar bindings and inlined helpers";goto fail;
        }
        if (!texel_append(&m.module.body,p,len)) goto fail;
    }
    if (!texel_append(&out,words,5)) goto fail;
    int inserted=0;
    for (size_t i=5;i<functions;i+=words[i]>>16) {
        unsigned op=words[i]&0xffff;
        if (!inserted && op>=19 && op<=39) {
            if (!texel_append(&out,annotations.w,annotations.n)) goto fail;
            inserted=1;
        }
        if (!texel_append(&out,words+i,words[i]>>16)) goto fail;
    }
    if (!inserted || m.module.next>0x3fffff || !texel_append(&out,m.module.globals.w,m.module.globals.n) ||
        !texel_append(&out,m.module.body.w,m.module.body.n)) goto fail;
    out.w[3]=m.module.next;*out_count=out.n;*reason=NULL;
    free(m.ids);free(ids);free(factors);free(annotations.w);free(m.module.globals.w);free(m.module.body.w);return out.w;
fail:
    free(m.ids);free(ids);free(factors);free(annotations.w);free(m.module.globals.w);free(m.module.body.w);free(out.w);return NULL;
}
#undef BORDER_OP
