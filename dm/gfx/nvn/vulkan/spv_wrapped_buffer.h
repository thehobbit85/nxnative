// A wrapped 2D fetch can address an NVN texture buffer. Lower that binding to
// a real texel buffer so storage aliases keep the same execution-time bytes.
#pragma once
#include "spv_texel_bias.h"

typedef struct {
    size_t declaration;
    uint32_t type, replacement, value;
    unsigned selected:1, affected:1, constant:1;
} wrapped_id;
typedef struct {
    const uint32_t *words;
    uint32_t bound;
    wrapped_id *ids;
    texel_module module;
} wrapped_module;
#define WRAP_OP(dst, op, ...) do { \
    uint32_t instruction[]={0,__VA_ARGS__}; size_t length=sizeof instruction/sizeof *instruction; \
    instruction[0]=(uint32_t)(length<<16)|(op); \
    if (!texel_append(dst,instruction,length)) goto fail; \
} while (0)

static uint32_t wrapped_type(wrapped_module *m, uint32_t type) {
    if (type>=m->bound || !m->ids[type].declaration) return 0;
    if (m->ids[type].replacement) return m->ids[type].replacement;
    const uint32_t *original=m->words+m->ids[type].declaration;
    unsigned length=original[0]>>16, op=original[0]&0xffff;
    uint32_t code[10];
    if (length>10) return 0;
    memcpy(code,original,length*4);
    if (op==25) {
        if (length<9 || code[3]!=1 || code[4] || code[5] || code[6] || code[7]!=1) return 0;
        code[3]=5; // DimBuffer
    } else if (op==27) {
        if (length!=3) return 0;
        return m->ids[type].replacement=wrapped_type(m,code[2]);
    } else if (op==32 || op==28 || op==29) {
        if (length!=(op==29 ? 3u : 4u)) return 0;
        unsigned child=op==32 ? 3 : 2;
        if (op==32 && code[2]!=0) return 0;
        code[child]=wrapped_type(m,code[child]);
        if (!code[child]) return 0;
    } else return 0;
    // Image and pointer types must be canonical even when a module already
    // declares a buffer alongside the 2D interface being adapted.
    for (uint32_t id=1;id<m->bound;id++) if (m->ids[id].declaration) {
        const uint32_t *candidate=m->words+m->ids[id].declaration;
        if (candidate[0]==code[0] && !memcmp(candidate+2,code+2,(length-2)*4))
            return m->ids[type].replacement=id;
    }
    for (size_t i=0;i<m->module.globals.n;i+=m->module.globals.w[i]>>16) {
        const uint32_t *candidate=m->module.globals.w+i;
        if (candidate[0]==code[0] && !memcmp(candidate+2,code+2,(length-2)*4))
            return m->ids[type].replacement=candidate[1];
    }
    code[1]=m->module.next++;
    if (!texel_append(&m->module.globals,code,length)) return 0;
    return m->ids[type].replacement=code[1];
}

static uint32_t wrapped_coordinate(wrapped_module *m,uint32_t value) {
    if (value>=m->bound || m->ids[value].type>=m->bound) return 0;
    size_t declaration=m->ids[m->ids[value].type].declaration;
    if (!declaration) return 0;
    const uint32_t *type=m->words+declaration;
    if ((type[0]&0xffff)!=23 || type[3]!=2 || type[2]>=m->bound) return 0;
    uint32_t scalar=type[2];
    const uint32_t *integer=m->words+m->ids[scalar].declaration;
    if ((integer[0]&0xffff)!=21 || integer[2]!=32) return 0;
    uint32_t width=texel_constant(&m->module,scalar,16384);
    if (!width) return 0;
    uint32_t x=m->module.next++, y=m->module.next++, row=m->module.next++, index=m->module.next++;
    WRAP_OP(&m->module.body,81,scalar,x,value,0);
    WRAP_OP(&m->module.body,81,scalar,y,value,1);
    WRAP_OP(&m->module.body,132,scalar,row,y,width);
    WRAP_OP(&m->module.body,128,scalar,index,x,row);
    return index;
fail:
    return 0;
}

static int wrapped_query(wrapped_module *m,const uint32_t *p) {
    unsigned op=p[0]&0xffff, length=p[0]>>16;
    if (p[1]>=m->bound || !m->ids[p[1]].declaration) return 0;
    if (op==106) {
        uint32_t one=texel_constant(&m->module,p[1],1);
        if (!one) return 0;
        WRAP_OP(&m->module.body,83,p[1],p[2],one);
        return 1;
    }
    if (op==103 && (length!=5 || p[4]>=m->bound || !m->ids[p[4]].constant || m->ids[p[4]].value)) return 0;
    const uint32_t *vector=m->words+m->ids[p[1]].declaration;
    if ((vector[0]&0xffff)!=23 || vector[0]>>16!=4 || vector[3]!=2 || vector[2]>=m->bound) return 0;
    uint32_t scalar=vector[2];
    const uint32_t *integer=m->words+m->ids[scalar].declaration;
    if ((integer[0]&0xffff)!=21 || integer[2]!=32) return 0;
    if (!m->module.boolean) {
        m->module.boolean=m->module.next++;
        WRAP_OP(&m->module.globals,20,m->module.boolean);
    }
    uint32_t width=texel_constant(&m->module,scalar,16384), last=texel_constant(&m->module,scalar,16383);
    if (!width || !last) return 0;
    uint32_t count=m->module.next++, small=m->module.next++, x=m->module.next++;
    uint32_t rounded=m->module.next++, y=m->module.next++;
    WRAP_OP(&m->module.body,104,scalar,count,p[3]);
    WRAP_OP(&m->module.body,176,m->module.boolean,small,count,width);
    WRAP_OP(&m->module.body,169,scalar,x,small,count,width);
    WRAP_OP(&m->module.body,128,scalar,rounded,count,last);
    WRAP_OP(&m->module.body,integer[3] ? 135 : 134,scalar,y,rounded,width);
    WRAP_OP(&m->module.body,80,p[1],p[2],x,y);
    return 1;
fail:
    return 0;
}

static uint32_t *spv_wrapped_buffer(const uint32_t *words,size_t count,const uint32_t *bindings,size_t nb,
                                   size_t *out_count,const char **reason) {
    *reason="invalid wrapped buffer module";
    if (count<5 || words[0]!=0x07230203 || !words[3] || words[3]>0x3fffff) return NULL;
    wrapped_module m={.words=words,.bound=words[3],.module={.next=words[3]}};
    texel_words prefix={0}, output={0};
    m.ids=calloc(m.bound,sizeof *m.ids);
    if (!m.ids) goto fail;
    size_t functions=count;
    int capability=0, nonuniform_images=0, nonuniform_buffers=0;size_t found=0;
    for (size_t i=5;i<count;) {
        const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        if (!length || length>count-i) goto fail;
        if (op==17 && length==2 && p[1]==46) capability=1;
        if (op==17 && length==2 && p[1]==5307) nonuniform_images=1;
        if (op==17 && length==2 && p[1]==5311) nonuniform_buffers=1;
        if (op>=19 && op<=33 && length>=2 && p[1]<m.bound) {
            m.ids[p[1]].declaration=i;
            if (op==20) m.module.boolean=p[1];
        } else if (length>=3 && p[1]<m.bound && m.ids[p[1]].declaration && p[2]<m.bound) {
            m.ids[p[2]].type=p[1];
            if (op==43 && length==4) { m.ids[p[2]].constant=1;m.ids[p[2]].value=p[3]; }
        }
        if (op==71 && length==4 && p[1]<m.bound && p[2]==33)
            for (size_t j=0;j<nb;j++) if (bindings[j]==p[3]) m.ids[p[1]].selected=1;
        if (op==59 && length>=4 && p[2]<m.bound && m.ids[p[2]].selected) {
            m.ids[p[2]].affected=1;found++;
        }
        if (op==54 && functions==count) functions=i;
        i+=length;
    }
    if (functions==count || found!=nb) goto fail;
    // Loads and copies retain the selected descriptor's identity. An image
    // passed into a function or merged with an ordinary 2D image is rejected.
    for (size_t i=functions;i<count;i+=words[i]>>16) {
        const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        if ((op==61 || op==65 || op==66 || op==83 || op==86 || op==100) && length>=4 && p[2]<m.bound && p[3]<m.bound)
            m.ids[p[2]].affected=m.ids[p[3]].affected;
        if (op==57) for (unsigned j=4;j<length;j++) if (p[j]<m.bound && m.ids[p[j]].affected) goto fail;
        if (op==169 || op==245) {
            unsigned start=op==169 ? 4 : 3, step=op==169 ? 1 : 2;
            for (unsigned j=start;j<length;j+=step) if (p[j]<m.bound && m.ids[p[j]].affected) goto fail;
        }
    }
    if (!texel_append(&output,words,5)) goto fail;
    if (!capability) WRAP_OP(&output,17,46); // SampledBuffer
    if (nonuniform_images && !nonuniform_buffers) WRAP_OP(&output,17,5311); // UniformTexelBufferArrayNonUniformIndexing
    for (size_t i=5;i<functions;i+=words[i]>>16) {
        const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        if (op==59 && length>=4 && p[2]<m.bound && m.ids[p[2]].affected) {
            uint32_t type=wrapped_type(&m,p[1]);
            if (!type || length!=4 || p[3]!=0) goto fail;
            WRAP_OP(&m.module.globals,59,type,p[2],p[3]);
        } else if (!texel_append(&prefix,p,length)) goto fail;
    }
    for (size_t i=functions;i<count;i+=words[i]>>16) {
        const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        int result=length>=3 && p[2]<m.bound && m.ids[p[2]].affected;
        int image=length>=4 && p[3]<m.bound && m.ids[p[3]].affected;
        if (result) {
            uint32_t type=wrapped_type(&m,p[1]);
            if (!type) goto fail;
            if (op==86 || op==100) WRAP_OP(&m.module.body,83,type,p[2],p[3]);
            else {
                uint32_t header[]={p[0],type};
                if (!texel_append(&m.module.body,header,2) || !texel_append(&m.module.body,p+2,length-2)) goto fail;
            }
        } else if (image && (op==95 || op==313)) {
            if (length!=5 && (length!=7 || p[5]!=2 || p[6]>=m.bound || !m.ids[p[6]].constant || m.ids[p[6]].value)) goto fail;
            uint32_t coordinate=wrapped_coordinate(&m,p[4]);
            if (!coordinate) goto fail;
            WRAP_OP(&m.module.body,op,p[1],p[2],p[3],coordinate);
        } else if (image && (op==103 || op==104 || op==106)) {
            if (!wrapped_query(&m,p)) goto fail;
        } else if (image && ((op>=87 && op<=107) || (op>=305 && op<=320))) {
            *reason="wrapped buffer requires fetch and size operations";goto fail;
        } else if (!texel_append(&m.module.body,p,length)) goto fail;
    }
    if (m.module.next>0x3fffff || !texel_append(&output,prefix.w,prefix.n) ||
        !texel_append(&output,m.module.globals.w,m.module.globals.n) ||
        !texel_append(&output,m.module.body.w,m.module.body.n)) goto fail;
    output.w[3]=m.module.next;*out_count=output.n;*reason=NULL;
    free(prefix.w);free(m.module.globals.w);free(m.module.body.w);free(m.ids);return output.w;
fail:
    free(prefix.w);free(m.module.globals.w);free(m.module.body.w);free(m.ids);free(output.w);return NULL;
}
#undef WRAP_OP
