// A fixed NVN descriptor array may alternate ordinary images and wrapped
// texture buffers. Keep both Vulkan interfaces, with a per-draw class map.
// The existing wrapped-buffer pass converts the second interface afterwards.
#pragma once
#include "spv_wrapped_buffer.h"
typedef struct { uint32_t binding,buffer_binding,flags_binding,count; } mixed_wrapped_edit;
typedef struct { uint32_t clone,root,index,active,flags,count; } mixed_wrapped_id;
#define MIXED_OP(dst, op, ...) do { \
    uint32_t ins[]={0,__VA_ARGS__};ins[0]=(uint32_t)(sizeof ins/4)<<16|(op); \
    if (!texel_append(dst,ins,sizeof ins/4)) goto fail; \
} while (0)

static uint32_t mixed_condition(wrapped_module *m,uint32_t type,uint32_t active) {
    if (type>=m->bound || !m->ids[type].declaration) return 0;
    const uint32_t *t=m->words+m->ids[type].declaration;
    if ((t[0]&0xffff)!=23) return ((t[0]&0xffff)==21 || (t[0]&0xffff)==22) ? active : 0;
    unsigned lanes=t[3];if (lanes<2 || lanes>4) return 0;
    uint32_t boolean=0;
    for (uint32_t id=1;id<m->bound;id++) if (m->ids[id].declaration) {
        const uint32_t *candidate=m->words+m->ids[id].declaration;
        if (candidate[0]==(4u<<16|23) && candidate[2]==m->module.boolean && candidate[3]==lanes) boolean=id;
    }
    for (size_t i=0;i<m->module.globals.n;i+=m->module.globals.w[i]>>16) {
        const uint32_t *candidate=m->module.globals.w+i;
        if (candidate[0]==(4u<<16|23) && candidate[2]==m->module.boolean && candidate[3]==lanes) boolean=candidate[1];
    }
    if (!boolean) { boolean=m->module.next++;MIXED_OP(&m->module.globals,23,boolean,m->module.boolean,lanes); }
    uint32_t result=m->module.next++,instruction[7]={(lanes+3)<<16|80,boolean,result,active,active,active,active};
    if (!texel_append(&m->module.body,instruction,lanes+3)) return 0;
    return result;
fail:
    return 0;
}
static uint32_t *spv_mixed_wrapped(const uint32_t *words,size_t count,const mixed_wrapped_edit *edits,size_t nedits,
                                  size_t *out_count,const char **reason) {
    *reason="invalid mixed wrapped-image module";*out_count=0;
    if (count<5 || words[0]!=0x07230203 || words[1]>0x10300 || !words[3] || words[3]>0x3fffff) return NULL;
    wrapped_module m={.words=words,.bound=words[3],.module={.next=words[3]}};
    mixed_wrapped_id *ids=calloc(m.bound,sizeof *ids);
    m.ids=calloc(m.bound,sizeof *m.ids);
    texel_words annotations={0},prefix={0},output={0};size_t functions=count,found=0;
    if (!ids || !m.ids) goto fail;
    for (size_t i=5;i<count;) {
        const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        if (!length || length>count-i) goto fail;
        if (op>=19 && op<=33 && length>=2 && p[1]<m.bound) {
            m.ids[p[1]].declaration=i;
            if (op==20) m.module.boolean=p[1];
            if (op==21 && length==4 && p[2]==32 && !p[3]) m.module.uint_type=p[1];
        } else if (length>=3 && p[1]<m.bound && m.ids[p[1]].declaration && p[2]<m.bound) m.ids[p[2]].type=p[1];
        if (op==71 && length==4 && p[1]<m.bound && p[2]==33) for (size_t e=0;e<nedits;e++)
            if (p[3]==edits[e].binding) { m.ids[p[1]].selected=1;ids[p[1]].root=(uint32_t)e+1; }
        if (op==59 && length==4 && p[2]<m.bound && m.ids[p[2]].selected) { m.ids[p[2]].affected=1;found++; }
        if (op==54 && length==5 && p[2]<m.bound) { m.ids[p[2]].declaration=i;if (functions==count) functions=i; }
        i+=length;
    }
    if (found!=nedits || functions==count || !wrapped_flow(&m,functions,count)) goto fail;
    for (size_t i=functions;i<count;i+=words[i]>>16) {
        const uint32_t *p=words+i;unsigned op=p[0]&0xffff;
        if ((op==55 || op==54) && m.ids[p[2]].affected) {
            *reason="mixed descriptor arrays require inlined shader helpers";goto fail;
        }
    }
    if (!m.module.boolean) { m.module.boolean=m.module.next++;MIXED_OP(&m.module.globals,20,m.module.boolean); }
    if (!m.module.uint_type) { m.module.uint_type=m.module.next++;MIXED_OP(&m.module.globals,21,m.module.uint_type,32,0); }
    uint32_t u32=m.module.uint_type,zero=texel_constant(&m.module,u32,0);
    uint32_t array=m.module.next++,structure=m.module.next++,pointer=m.module.next++,element_pointer=m.module.next++;
    MIXED_OP(&m.module.globals,29,array,u32);
    MIXED_OP(&m.module.globals,30,structure,array);
    MIXED_OP(&m.module.globals,32,pointer,12,structure);
    MIXED_OP(&m.module.globals,32,element_pointer,12,u32);
    MIXED_OP(&annotations,71,array,6,4);
    MIXED_OP(&annotations,71,structure,2);
    MIXED_OP(&annotations,72,structure,0,35,0);
    MIXED_OP(&annotations,72,structure,0,24);
    for (uint32_t id=1;id<m.bound;id++) if (m.ids[id].affected) ids[id].clone=m.module.next++;
    for (size_t i=5;i<functions;i+=words[i]>>16) {
        const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        if (op==59 && length==4 && m.ids[p[2]].selected) {
            mixed_wrapped_id *id=&ids[p[2]];const mixed_wrapped_edit *e=&edits[id->root-1];
            if (!e->count || e->count>32 || p[3]!=0) goto fail;
            id->index=zero;id->flags=m.module.next++;id->count=e->count;
            MIXED_OP(&m.module.globals,59,p[1],id->clone,p[3]);
            MIXED_OP(&m.module.globals,59,pointer,id->flags,12);
            MIXED_OP(&annotations,71,id->clone,34,0);
            MIXED_OP(&annotations,71,id->clone,33,e->buffer_binding);
            MIXED_OP(&annotations,71,id->flags,34,0);
            MIXED_OP(&annotations,71,id->flags,33,e->flags_binding);
        }
        if (op==71 && length==3 && p[2]==5300 && p[1]<m.bound && ids[p[1]].clone)
            MIXED_OP(&annotations,71,ids[p[1]].clone,5300);
        if (!texel_append(&prefix,p,length)) goto fail;
    }
    for (size_t i=functions;i<count;i+=words[i]>>16) {
        const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        int affected=length>=3 && p[2]<m.bound && m.ids[p[2]].affected;
        int image=length>=4 && p[3]<m.bound && m.ids[p[3]].affected;
        if (affected) {
            if ((op!=61 && op!=65 && op!=66 && op!=83 && op!=86 && op!=100) || !image || length>8) {
                *reason="mixed descriptor flow needs direct loads, indexing or copies";goto fail;
            }
            mixed_wrapped_id *dest=&ids[p[2]],source=ids[p[3]];
            if (!source.root) goto fail;
            uint32_t clone[8];memcpy(clone,p,length*4);clone[2]=dest->clone;clone[3]=source.clone;
            dest->root=source.root;dest->index=source.index;dest->active=source.active;dest->flags=source.flags;dest->count=source.count;
            if (op==65 || op==66) {
                if (length!=5 || p[4]>=m.bound) goto fail;
                dest->index=m.module.next++;MIXED_OP(&m.module.body,124,u32,dest->index,p[4]);
            }
            if (!texel_append(&m.module.body,p,length) || !texel_append(&m.module.body,clone,length)) goto fail;
            if (op==61) {
                uint32_t limit=texel_constant(&m.module,u32,source.count);
                uint32_t inside=m.module.next++,bounded=m.module.next++,address=m.module.next++,flag=m.module.next++;
                dest->active=m.module.next++;
                MIXED_OP(&m.module.body,176,m.module.boolean,inside,source.index,limit);
                MIXED_OP(&m.module.body,169,u32,bounded,inside,source.index,limit);
                MIXED_OP(&m.module.body,65,element_pointer,address,source.flags,zero,bounded);
                MIXED_OP(&m.module.body,61,u32,flag,address);
                MIXED_OP(&m.module.body,171,m.module.boolean,dest->active,flag,zero);
            }
        } else if (image && (op==95 || op==103 || op==104 || op==106)) {
            if (length>8 || !ids[p[3]].active) goto fail;
            uint32_t original[8],buffer[8];memcpy(original,p,length*4);memcpy(buffer,p,length*4);
            original[2]=m.module.next++;buffer[2]=m.module.next++;buffer[3]=ids[p[3]].clone;
            uint32_t active=ids[p[3]].active;
            if (op==95) {
                uint32_t type=p[4]<m.bound ? m.ids[p[4]].type : 0;
                uint32_t condition=mixed_condition(&m,type,active),null=m.module.next++;
                if (!condition) goto fail;
                MIXED_OP(&m.module.globals,46,type,null);
                original[4]=m.module.next++;buffer[4]=m.module.next++;
                // Both fetches are valid even without robust image access:
                // the inactive representation fetches zero from its dummy.
                MIXED_OP(&m.module.body,169,type,original[4],condition,null,p[4]);
                MIXED_OP(&m.module.body,169,type,buffer[4],condition,p[4],null);
            }
            if (!texel_append(&m.module.body,original,length) || !texel_append(&m.module.body,buffer,length)) goto fail;
            uint32_t condition=mixed_condition(&m,p[1],active);if (!condition) goto fail;
            MIXED_OP(&m.module.body,169,p[1],p[2],condition,buffer[2],original[2]);
        } else if (image && ((op>=87 && op<=107) || (op>=305 && op<=320))) {
            *reason="mixed wrapped arrays require ordinary fetch and size operations";goto fail;
        } else if (!texel_append(&m.module.body,p,length)) goto fail;
    }
    if (!texel_append(&output,words,5)) goto fail;
    int annotated=0;
    for (size_t i=0;i<prefix.n;i+=prefix.w[i]>>16) {
        unsigned op=prefix.w[i]&0xffff;
        if (!annotated && op>=19 && op<=39) { if (!texel_append(&output,annotations.w,annotations.n)) goto fail;annotated=1; }
        if (!texel_append(&output,prefix.w+i,prefix.w[i]>>16)) goto fail;
    }
    if (!annotated || !texel_append(&output,m.module.globals.w,m.module.globals.n) || !texel_append(&output,m.module.body.w,m.module.body.n)) goto fail;
    output.w[3]=m.module.next;*out_count=output.n;*reason=NULL;
    free(ids);free(m.ids);free(annotations.w);free(prefix.w);free(m.module.globals.w);free(m.module.body.w);return output.w;
fail:
    free(ids);free(m.ids);free(annotations.w);free(prefix.w);free(m.module.globals.w);free(m.module.body.w);free(output.w);return NULL;
}
#undef MIXED_OP
