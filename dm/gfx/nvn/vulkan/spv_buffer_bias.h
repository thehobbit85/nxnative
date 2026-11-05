// A descriptor may start before its NVN range to meet Vulkan's offset alignment.
// Give that variable a private block type whose members start `bias` bytes later.
// Member types, access chains, atomics and runtime-array length then keep their
// original meaning; two bindings of one block type can have different biases.
#pragma once
#include <stdint.h>
#include "resource_contract.h"
#include <stdlib.h>
#include <string.h>

typedef struct { size_t at; uint32_t type, binding; uint8_t has_binding, block; } buffer_bias_id;
typedef struct { uint32_t var, block, pointer, bias, new_block, new_pointer; } buffer_bias_edit;

static uint32_t *spv_buffer_bias(const uint32_t *w, size_t count, const uint32_t *bindings,
                                 const uint32_t *biases, size_t nbindings, size_t *out_count, const char **reason) {
    enum { TypeStruct=30, TypePointer=32, Function=54, Call=57, Variable=59, Load=61, Store=62,
           CopyMemory=63, CopyMemorySized=64, AccessChain=65, InBoundsAccessChain=66, PtrAccessChain=67,
           ArrayLength=68, InBoundsPtrAccessChain=70, Decorate=71, MemberDecorate=72, CopyObject=83,
           Select=169, Phi=245, Block=2, BufferBlock=3, Binding=33, Offset=35 };
    *reason = "invalid buffer-bias SPIR-V";
    if (count<5 || w[0]!=0x07230203 || !w[3] || w[3]>0x3fffff || nbindings>DM_VK_MAX_BINDINGS || count>SIZE_MAX/sizeof(uint32_t)/128) return NULL;
    uint32_t bound=w[3];
    buffer_bias_id *ids=calloc(bound,sizeof *ids);
    buffer_bias_edit edits[DM_VK_MAX_BINDINGS]; size_t ne=0, annotations=count, extra=0;
    uint32_t *out=NULL;
    if (!ids) { *reason="out of memory"; return NULL; }
    for (size_t i=5;i<count;) {
        uint32_t len=w[i]>>16, op=w[i]&0xffff;
        if (!len || len>count-i) goto fail;
        if (op>=19 && op<=39 && annotations==count) annotations=i;
        if ((op==TypeStruct || op==TypePointer) && len>=2 && w[i+1]<bound) ids[w[i+1]].at=i;
        if (op==Variable && len>=4 && w[i+2]<bound) { ids[w[i+2]].at=i; ids[w[i+2]].type=w[i+1]; }
        if (op==Decorate && len>=3 && w[i+1]<bound) {
            if (w[i+2]==Binding && len==4) { ids[w[i+1]].binding=w[i+3]; ids[w[i+1]].has_binding=1; }
            if (w[i+2]==Block || w[i+2]==BufferBlock) ids[w[i+1]].block=(uint8_t)w[i+2];
        }
        i+=len;
    }
    for (size_t b=0;b<nbindings;b++) if (biases[b]) {
        uint32_t var=0;
        for (uint32_t v=1;v<bound;v++) if (ids[v].has_binding && ids[v].binding==bindings[b] && ids[v].type) { var=v; break; }
        if (!var || ids[var].type>=bound) { *reason="buffer binding not found"; goto fail; }
        uint32_t ptr=ids[var].type;
        size_t pi=ids[ptr].at;
        if (!pi || (w[pi]&0xffff)!=TypePointer || w[pi]>>16!=4 || w[pi+3]>=bound) goto fail;
        uint32_t block=w[pi+3]; size_t bi=ids[block].at;
        if (!bi || (w[bi]&0xffff)!=TypeStruct || !ids[block].block ||
            !(w[pi+2]==12 || (w[pi+2]==2 && ids[block].block==BufferBlock))) {
            *reason="storage binding is not a direct buffer block"; goto fail;
        }
        edits[ne]=(buffer_bias_edit){var,block,ptr,biases[b],bound+(uint32_t)(2*ne),bound+(uint32_t)(2*ne+1)};
        extra+=(w[bi]>>16)+4;
        ne++;
    }
    int functions=0;
    for (size_t i=5;i<count;) {
        uint32_t len=w[i]>>16, op=w[i]&0xffff;
        if (op==Function) functions=1;
        for (size_t e=0;e<ne;e++) {
            const buffer_bias_edit *r=&edits[e];
            if ((op==Decorate || op==MemberDecorate) && len>=3 && w[i+1]==r->block) {
                extra+=len;
                if (op==MemberDecorate && len==5 && w[i+3]==Offset && w[i+4]>UINT32_MAX-r->bias) goto fail;
            }
            // Whole-block pointers escaping to a function or aggregate load need
            // type propagation. NVN's translated word-addressed blocks use member
            // access chains; refuse other uses instead of creating invalid SPIR-V.
            if (functions) {
                size_t from=0, to=0;
                if ((op==Load || op==CopyObject || op==PtrAccessChain || op==InBoundsPtrAccessChain) && len>=4) { from=3;to=4; }
                if (op==Store && len>=3) { from=1;to=2; }
                if ((op==CopyMemory || op==CopyMemorySized) && len>=3) { from=1;to=3; }
                if (op==Call) { from=4;to=len; }
                if (op==Select) { from=4;to=len; }
                if (op==Phi) { from=3;to=len; }
                for (size_t j=from;j<to;j++) if (w[i+j]==r->var) { *reason="buffer block pointer escapes member access"; goto fail; }
            }
        }
        i+=len;
    }
    if (count>SIZE_MAX/sizeof *out-extra) goto fail;
    out=malloc((count+extra)*sizeof *out);
    if (!out) { *reason="out of memory"; goto fail; }
    memcpy(out,w,5*sizeof *out);size_t n=5;
    for (size_t i=5;i<count;) {
        uint32_t len=w[i]>>16, op=w[i]&0xffff;
        if (i==annotations) for (size_t e=0;e<ne;e++) {
            const buffer_bias_edit *r=&edits[e];
            for (size_t j=5;j<count;) {
                uint32_t jl=w[j]>>16, jo=w[j]&0xffff;
                if ((jo==Decorate || jo==MemberDecorate) && jl>=3 && w[j+1]==r->block) {
                    memcpy(out+n,w+j,jl*sizeof *out);out[n+1]=r->new_block;
                    if (jo==MemberDecorate && jl==5 && w[j+3]==Offset) out[n+4]+=r->bias;
                    n+=jl;
                }
                j+=jl;
            }
        }
        const buffer_bias_edit *edit=NULL;
        if (op==Variable && len>=4) for (size_t e=0;e<ne;e++) if (w[i+2]==edits[e].var) { edit=&edits[e];break; }
        if (edit) {
            size_t at=ids[edit->block].at, bl=w[at]>>16;
            memcpy(out+n,w+at,bl*sizeof *out);out[n+1]=edit->new_block;n+=bl;
            at=ids[edit->pointer].at;
            memcpy(out+n,w+at,4*sizeof *out);out[n+1]=edit->new_pointer;out[n+3]=edit->new_block;n+=4;
        }
        memcpy(out+n,w+i,len*sizeof *out);
        if (edit) out[n+1]=edit->new_pointer;
        n+=len;i+=len;
    }
    out[3]=bound+(uint32_t)(2*ne);*out_count=n;*reason=NULL;free(ids);return out;
fail:
    free(out);free(ids);return NULL;
}
