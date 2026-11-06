// Maxwell writes raw register bits to integer color attachments. Change only
// the SPIR-V interface and bitcast at its loads/stores. Arithmetic inside the
// fragment program, including reads of values it wrote earlier, keeps its type.
#pragma once
#include "spv_fragment_outputs.h"
#include <string.h>

static uint32_t *spv_integer_outputs(const uint32_t *w, size_t nw, uint8_t uint_mask, uint8_t sint_mask,
                                     size_t *result_words, const char **reason) {
    enum { Variable=59, Load=61, Store=62, CopyMemory=63, CopyMemorySized=64, AccessChain=65,
        InBoundsAccessChain=66, PtrAccessChain=67, InBoundsPtrAccessChain=70, CopyObject=83, Bitcast=124,
        FunctionCall=57, Select=169, Phi=245, TypeInt=21, TypeVector=23, TypePointer=32, Output=3 };
    *reason="unsupported integer fragment output interface";
    fragment_outputs interface;
    size_t fn=0;
    fragment_id *id=spv_output_ids(w,nw,&interface,&fn);
    if (!id) return NULL;
    uint32_t *out=NULL;
    if (nw>(SIZE_MAX/4-160)/3 || (uint_mask&sint_mask)) goto fail;
    out=malloc((3*nw+160)*sizeof *out);
    if (!out) { *reason="out of memory"; goto fail; }
    uint32_t old_bound=w[3], bound=old_bound, type[3][5]={{0}}, ptr[3][5]={{0}};
    uint8_t needed[3][5]={{0}};
    for (uint32_t i=1;i<old_bound;++i) {
        fragment_id t=id[i];
        if (t.bits==32 && t.kind && t.lanes) type[t.kind][t.lanes]=i;
        if (t.variable || !t.type) continue;
        fragment_id value=id[t.type];
        if (value.bits==32 && value.kind && value.lanes) ptr[value.kind][value.lanes]=i;
    }
    uint8_t matched=0;
    for (uint32_t i=1;i<old_bound;++i) {
        fragment_id v=id[i], t=id[v.type];
        if (!v.variable || v.builtin || !v.located || v.location>=8) continue;
        uint32_t bit=1u<<v.location;
        uint8_t kind=(uint_mask&bit) ? OUT_UINT : (sint_mask&bit) ? OUT_SINT : 0;
        if (!kind) continue;
        matched|=bit;
        if (t.bits && kind==t.kind) continue;
        if (t.bits!=32 || !t.lanes) goto fail;
        id[i].cast=kind; needed[kind][1]=needed[kind][t.lanes]=1;
    }
    if (matched!=(uint_mask|sint_mask)) goto fail;
    // Delay the selected variables until all existing numeric types have
    // been declared. Reusing them avoids duplicate scalar/vector type IDs.
    memcpy(out,w,5*sizeof *w);
    size_t at=5;
    for (size_t i=5;i<fn;i+=w[i]>>16) {
        unsigned op=w[i]&0xffff, n=w[i]>>16;
        if (op==Variable && n>=4 && w[i+2]<old_bound && id[w[i+2]].cast) {
            if (n!=4) { *reason="integer output has a global initializer"; goto fail; }
            continue;
        }
        memcpy(out+at,w+i,n*sizeof *w); at+=n;
    }
#define FRAG_EMIT(op, ...) do { const uint32_t args[]={__VA_ARGS__}; size_t count=sizeof args/sizeof *args; \
    out[at++]=((uint32_t)(1+count)<<16)|(op); memcpy(out+at,args,sizeof args); at+=count; } while (0)
    for (unsigned kind=OUT_UINT;kind<=OUT_SINT;++kind) for (unsigned lanes=1;lanes<=4;++lanes) {
        if (!needed[kind][lanes]) continue;
        if (!type[kind][lanes]) {
            type[kind][lanes]=bound++;
            if (lanes==1) FRAG_EMIT(TypeInt,type[kind][1],32,kind==OUT_SINT);
            else FRAG_EMIT(TypeVector,type[kind][lanes],type[kind][1],lanes);
        }
        if (!ptr[kind][lanes]) {
            ptr[kind][lanes]=bound++;
            FRAG_EMIT(TypePointer,ptr[kind][lanes],Output,type[kind][lanes]);
        }
    }
    for (uint32_t i=1;i<old_bound;++i) if (id[i].variable && id[i].cast)
        FRAG_EMIT(Variable,ptr[id[i].cast][id[id[i].type].lanes],i,Output);
    for (size_t i=fn;i<nw;) {
        const uint32_t *p=w+i;
        unsigned op=p[0]&0xffff, n=p[0]>>16;
        if (!n || n>nw-i) goto fail;
        size_t start=at;
        unsigned kind=0;
        if ((op==AccessChain || op==InBoundsAccessChain || op==CopyObject) && n>=4 && p[3]<old_bound &&
            (kind=id[p[3]].cast)) {
            if (p[1]>=old_bound || p[2]>=old_bound) goto fail;
            uint32_t pointee=id[p[1]].type, dest=ptr[kind][id[pointee].lanes];
            if (!dest) goto fail;
            id[p[2]].type=pointee; id[p[2]].cast=(uint8_t)kind;
            memcpy(out+at,p,n*sizeof *p); at+=n; out[start+1]=dest;
        } else if (op==Load && n>=4 && p[3]<old_bound && (kind=id[p[3]].cast)) {
            uint32_t raw=bound++;
            if (p[1]>=old_bound || !type[kind][id[p[1]].lanes]) goto fail;
            memcpy(out+at,p,n*sizeof *p); at+=n;
            out[start+1]=type[kind][id[p[1]].lanes]; out[start+2]=raw;
            FRAG_EMIT(Bitcast,p[1],p[2],raw);
        } else if (op==Store && n>=3 && p[1]<old_bound && (kind=id[p[1]].cast)) {
            uint32_t raw=bound++;
            FRAG_EMIT(Bitcast,type[kind][id[id[p[1]].type].lanes],raw,p[2]);
            memcpy(out+at,p,n*sizeof *p); out[at+2]=raw; at+=n;
        } else {
            // A pointer passed between functions or merged from two branches
            // would also require changing its signature or its other inputs.
            unsigned first=n;
            if (op==CopyMemory || op==CopyMemorySized) first=1;
            if (op==PtrAccessChain || op==InBoundsPtrAccessChain || op==Phi || op==Select) first=3;
            if (op==FunctionCall) first=4;
            for (unsigned j=first;j<n;++j) if (p[j]<old_bound && id[p[j]].cast) {
                *reason="integer output pointer escapes its declaration"; goto fail;
            }
            memcpy(out+at,p,n*sizeof *p); at+=n;
        }
        i+=n;
    }
#undef FRAG_EMIT
    if (bound>0x3fffff) goto fail;
    free(id); out[3]=bound; *result_words=at; *reason=NULL; return out;
fail:
    free(out); free(id); return NULL;
}
