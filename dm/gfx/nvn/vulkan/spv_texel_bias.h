// Vulkan can require a typed view to start before its guest address. Add the
// prefix in texels at image accesses, and remove it from image size queries.
// Each descriptor-array element can point at a differently aligned address.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t binding, element, texels; } texel_bias;
typedef struct {
    uint32_t type, binding, index, value;
    unsigned integer:1, constant:1, affected:1, variable:1;
} texel_id;
typedef struct { uint32_t *w; size_t n, cap; } texel_words;

static int texel_append(texel_words *dst, const uint32_t *words, size_t n) {
    if (n>SIZE_MAX/sizeof *words-dst->n) return 0;
    size_t end=dst->n+n;
    if (end>dst->cap) {
        size_t cap=end;
        if (dst->cap<=SIZE_MAX/(2*sizeof *words) && cap<dst->cap*2) cap=dst->cap*2;
        if (cap>SIZE_MAX/sizeof *words) return 0;
        uint32_t *w=realloc(dst->w,cap*sizeof *w);
        if (!w) return 0;
        dst->w=w; dst->cap=cap;
    }
    memcpy(dst->w+dst->n,words,n*sizeof *words); dst->n=end;
    return 1;
}

typedef struct {
    texel_words globals, body;
    uint32_t next, boolean;
} texel_module;

#define TEXEL_OP(dst, op, ...) do { \
    uint32_t ins[]={0,__VA_ARGS__}; size_t len=sizeof ins/sizeof *ins; \
    ins[0]=(uint32_t)(len<<16)|(op); if (!texel_append(dst,ins,len)) goto fail; \
} while (0)

static uint32_t texel_constant(texel_module *m,uint32_t type,uint32_t value) {
    for (size_t i=0;i<m->globals.n;i+=m->globals.w[i]>>16) {
        const uint32_t *p=m->globals.w+i;
        if ((p[0]&0xffff)==43 && p[1]==type && p[3]==value) return p[2];
    }
    uint32_t id=m->next++;
    TEXEL_OP(&m->globals,43,type,id,value);
    return id;
fail:
    return 0;
}

// Return an ID with the bias selected by the original descriptor index. Zero
// means this particular constant subscript uses an already aligned descriptor.
static int texel_operand_bias(texel_module *m, const texel_bias *edits, size_t count,
                              const texel_id *ids, texel_id image, uint32_t type, uint32_t *bias) {
    *bias=0;
    const texel_id index=ids[image.index];
    int dynamic=image.index && !index.constant;
    if (dynamic && !index.type) return 0;
    for (size_t i=0;i<count;i++) {
        const texel_bias *e=&edits[i];
        if (e->binding!=image.binding || (!dynamic && e->element!=index.value)) continue;
        uint32_t amount=texel_constant(m,type,e->texels);
        if (!amount) return 0;
        if (!dynamic) { *bias=amount; return 1; }
        if (!*bias) *bias=texel_constant(m,type,0);
        uint32_t element=texel_constant(m,index.type,e->element);
        if (!*bias || !element) return 0;
        uint32_t match=m->next++, selected=m->next++;
        TEXEL_OP(&m->body,170,m->boolean,match,image.index,element); // OpIEqual
        TEXEL_OP(&m->body,169,type,selected,match,amount,*bias);
        *bias=selected;
    }
    return 1;
fail:
    return 0;
}

static uint32_t *spv_texel_bias(const uint32_t *w, size_t count, const texel_bias *edits,
                                size_t ne, size_t *out_count, const char **reason) {
    enum { TypeBool=20, TypeInt=21, Constant=43, Function=54, FunctionCall=57, Variable=59, ImageTexelPointer=60,
           Load=61, Store=62, CopyMemory=63, CopyMemorySized=64, AccessChain=65, InBoundsAccessChain=66,
           PtrAccessChain=67, InBoundsPtrAccessChain=70, Decorate=71, CopyObject=83,
           SampledImage=86, ImageFetch=95, ImageRead=98, ImageWrite=99, Image=100, ImageQuerySize=104,
           Select=169, Phi=245, ReturnValue=254, ImageSparseFetch=313, ImageSparseRead=320 };
    *reason="invalid texel-bias module";
    if (count<5 || w[0]!=0x07230203 || !w[3] || w[3]>0x3fffff) return NULL;
    uint32_t bound=w[3];
    texel_id *ids=calloc(bound,sizeof *ids);
    uint8_t *found=calloc(ne,1);
    texel_module m={.next=bound};
    texel_words out={0}; size_t functions=count;
    if (!ids || !found) goto fail;
    for (size_t i=5;i<count;) {
        const uint32_t *p=w+i; uint32_t len=p[0]>>16, op=p[0]&0xffff;
        if (!len || len>count-i) goto fail;
        if (op==Variable && len>=4 && p[2]<bound && p[3]==0) ids[p[2]].variable=1; // UniformConstant
        if (op==TypeBool && len==2) m.boolean=p[1];
        if (op==TypeInt && len==4 && p[1]<bound && p[2]==32) ids[p[1]].integer=1;
        // A scalar result uses its integer type ID as its first operand.
        if (len>=3 && p[1]<bound && ids[p[1]].integer && p[2]<bound && op!=TypeInt) {
            ids[p[2]].type=p[1];
            if (op==Constant && len==4) { ids[p[2]].constant=1; ids[p[2]].value=p[3]; }
        }
        if (op==Decorate && len==4 && p[1]<bound && p[2]==33) {
            for (size_t j=0;j<ne;j++) if (p[3]==edits[j].binding) {
                ids[p[1]].binding=p[3]; ids[p[1]].affected=1; found[j]=1;
            }
        }
        if (op==Function && functions==count) functions=i;
        i+=len;
    }
    for (size_t i=0;i<ne;i++) if (!found[i]) { *reason="typed binding absent from module"; goto fail; }
    for (uint32_t i=1;i<bound;i++) if (ids[i].affected && !ids[i].variable) { *reason="indirect typed descriptor decoration"; goto fail; }
    if (functions==count) goto fail;
    if (!m.boolean) { m.boolean=m.next++; TEXEL_OP(&m.globals,TypeBool,m.boolean); }
    for (size_t i=functions;i<count;) {
        const uint32_t *p=w+i; uint32_t len=p[0]>>16, op=p[0]&0xffff;
        if ((op==Load || op==CopyObject || op==Image || op==SampledImage || op==AccessChain ||
             op==InBoundsAccessChain) && len>=4 && p[2]<bound && p[3]<bound && ids[p[3]].affected) {
            texel_id src=ids[p[3]];
            if (op==AccessChain || op==InBoundsAccessChain) {
                if (len!=5 || src.index || p[4]>=bound) { *reason="nested typed descriptor array"; goto fail; }
                src.index=p[4];
            }
            ids[p[2]]=src;
        }
        int query=op==ImageQuerySize;
        unsigned operand=op==ImageWrite ? 1 : query || op==ImageFetch || op==ImageRead || op==ImageTexelPointer ||
            op==ImageSparseFetch || op==ImageSparseRead ? 3 : len;
        if (operand<len && p[operand]<bound && ids[p[operand]].affected) {
            uint32_t type=query ? p[1] : operand+1<len && p[operand+1]<bound ? ids[p[operand+1]].type : 0;
            if (!type || type>=bound || !ids[type].integer) { *reason="typed image needs a 32-bit coordinate or size"; goto fail; }
            uint32_t bias;
            if (!texel_operand_bias(&m,edits,ne,ids,ids[p[operand]],type,&bias)) goto fail;
            if (bias && query) {
                uint32_t raw=m.next++;
                TEXEL_OP(&m.body,ImageQuerySize,type,raw,p[operand]);
                TEXEL_OP(&m.body,130,type,p[2],raw,bias); // OpISub
                i+=len; continue;
            }
            if (bias) {
                uint32_t coord=p[operand+1], sum=m.next++, wrap=m.next++, adjusted=m.next++;
                TEXEL_OP(&m.body,128,type,sum,coord,bias); // OpIAdd
                // A negative or large unsigned coordinate must never wrap
                // around and acquire access to the hidden prefix.
                TEXEL_OP(&m.body,176,m.boolean,wrap,sum,bias); // OpULessThan
                TEXEL_OP(&m.body,Select,type,adjusted,wrap,coord,sum);
                size_t start=m.body.n;
                if (!texel_append(&m.body,p,len)) goto fail;
                m.body.w[start+operand+1]=adjusted;
                i+=len; continue;
            }
        }
        if (!texel_append(&m.body,p,len)) goto fail;
        i+=len;
    }
    // Check escapes after resolving all direct origins: an OpPhi may refer to
    // a handle from a back edge that appears later in the instruction stream.
    for (size_t i=functions;i<count;) {
        const uint32_t *p=w+i; unsigned len=p[0]>>16, op=p[0]&0xffff;
        unsigned from=len, end=len;
        switch (op) {
        case FunctionCall: case Select: from=4; break;
        case Phi: from=3; break;
        case Store: case CopyMemory: case CopyMemorySized: from=1; end=3; break;
        case PtrAccessChain: case InBoundsPtrAccessChain: from=3; end=4; break;
        case ReturnValue: from=1; break;
        }
        if (end>len) goto fail;
        for (unsigned j=from;j<end;j++) if (p[j]<bound && ids[p[j]].affected) {
            *reason="typed descriptor escapes its binding"; goto fail;
        }
        i+=len;
    }
    if (m.next>0x3fffff || !texel_append(&out,w,functions) ||
        (m.globals.n && !texel_append(&out,m.globals.w,m.globals.n)) || !texel_append(&out,m.body.w,m.body.n)) goto fail;
    out.w[3]=m.next; *out_count=out.n; *reason=NULL;
    free(m.body.w); free(m.globals.w); free(found); free(ids); return out.w;
fail:
    free(out.w); free(m.body.w); free(m.globals.w); free(found); free(ids); return NULL;
}
#undef TEXEL_OP
