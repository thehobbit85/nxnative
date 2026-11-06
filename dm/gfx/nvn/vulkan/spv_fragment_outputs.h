// The Maxwell exporter declares standalone numeric fragment outputs. Reflect
// their locations before choosing attachment operations and shader variants.
#pragma once
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint32_t alpha_var, alpha_vec, alpha_scalar;
    uint8_t targets;
} fragment_outputs;

static fragment_outputs spv_fragment_outputs(const uint32_t *w, size_t nw) {
    enum { EntryPoint=15, TypeInt=21, TypeFloat=22, TypeVector=23, TypePointer=32, Function=54,
        Variable=59, Decorate=71, GroupDecorate=74, GroupMemberDecorate=75, Output=3 };
    typedef struct {
        uint32_t type, element;
        uint8_t numeric, floating, lanes, location, located, component, index, builtin;
    } output_id;
    // If an unfamiliar aggregate or grouped decoration could span several
    // render targets, keep the guest write masks for those pipelines.
    const fragment_outputs unknown={.targets=UINT8_MAX};
    if (nw<5 || w[0]!=0x07230203 || !w[3] || w[3]>0x3fffff) return unknown;
    uint32_t bound=w[3];
    output_id *id=calloc(bound,sizeof *id);
    if (!id) return unknown;
    fragment_outputs out={0};
    unsigned entries=0;
    for (size_t i=5;i<nw;) {
        const uint32_t *p=w+i;
        unsigned op=p[0]&0xffff, n=p[0]>>16;
        if (!n || n>nw-i || op==GroupDecorate || op==GroupMemberDecorate) goto unknown_interface;
        if (op==Function) break;
        if (op==EntryPoint && (n<4 || p[1]!=4 || ++entries!=1)) goto unknown_interface;
        if ((op==TypeFloat || op==TypeInt) && n>=3 && p[1]<bound) {
            id[p[1]].numeric=p[2]==16 || p[2]==32 || p[2]==8;
            id[p[1]].floating=op==TypeFloat && (p[2]==16 || p[2]==32);
        }
        if (op==TypeVector && n==4 && p[1]<bound && p[2]<bound && p[3]<=4) {
            id[p[1]].element=p[2]; id[p[1]].lanes=(uint8_t)p[3];
            id[p[1]].numeric=id[p[2]].numeric;
        }
        if (op==TypePointer && n==4 && p[1]<bound && p[3]<bound && p[2]==Output) id[p[1]].type=p[3];
        if (op==Decorate && n==4 && p[1]<bound) {
            output_id *v=id+p[1];
            switch (p[2]) {
                case 11: v->builtin=1; break;
                case 30: v->located=1; v->location=p[3]<8 ? (uint8_t)p[3] : 8; break;
                case 31: v->component=!!p[3]; break;
                case 32: v->index=!!p[3]; break;
            }
        }
        if (op==Variable && n>=4 && p[3]==Output && p[1]<bound && p[2]<bound) {
            const output_id *v=id+p[2];
            uint32_t type=id[p[1]].type;
            if (!v->builtin && !v->index) {
                if (!v->located || !id[type].numeric) out.targets=UINT8_MAX;
                else if (v->location<8) out.targets|=(uint8_t)(1u<<v->location);
                uint32_t element=id[type].element;
                if (v->located && !v->location && !v->component && id[type].lanes==4 && id[element].floating) {
                    out.alpha_var=p[2]; out.alpha_vec=type; out.alpha_scalar=element;
                }
            }
        }
        i+=n;
    }
    free(id); return entries==1 ? out : unknown;
unknown_interface:
    free(id); return unknown;
}
