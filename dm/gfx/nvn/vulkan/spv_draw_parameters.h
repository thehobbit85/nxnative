// shaderDrawParameters is optional even on Vulkan 1.3 (SwiftShader lacks it).
// BaseVertex, BaseInstance and DrawIndex can come from host push words. A
// fourth word restores the vertex index removed by compact vertex staging.
// Guest NVN push constants use descriptors.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t *spv_draw_parameters(const uint32_t *w, size_t nw, int native, size_t *out_n, int *ok) {
    enum { Name=5, Extension=10, EntryPoint=15, Capability=17, TypeInt=21,
           TypeStruct=30, TypePointer=32, Constant=43, Function=54, Variable=59,
           Load=61, AccessChain=65, Decorate=71, MemberDecorate=72, Bitcast=124, IAdd=128,
           Input=1, PushConstant=9, BuiltIn=11, Block=2, Offset=35, DrawParameters=4427 };
    *ok=0;
    if (nw<5 || w[0]!=0x07230203 || !w[3] || w[3]>0x3fffff || nw>(SIZE_MAX/4-128)/6) return NULL;
    uint32_t bound=w[3], u32=0; size_t first_type=0, first_fn=0;
    int cap=0, entries=0, vertex=0, existing_push=0, count=0;
    uint8_t *member=calloc(bound,1), *integer=calloc(bound,1);
    uint32_t *pointee=calloc(bound,sizeof *pointee);
    if (!member || !integer || !pointee) goto fail;
    for (size_t i=5;i<nw;) {
        uint32_t op=w[i]&0xffff,n=w[i]>>16;
        if (!n || n>nw-i) goto fail;
        if (op==Capability && n==2 && w[i+1]==DrawParameters) cap=1;
        if (op==EntryPoint && n>=4) { entries++; vertex=w[i+1]==0; }
        if (op>=19 && op<=39 && !first_type) first_type=i;
        if (op==Function && !first_fn) first_fn=i;
        if (op==TypeInt && n==4 && w[i+1]<bound && w[i+2]==32) {
            integer[w[i+1]]=1; if (!w[i+3]) u32=w[i+1];
        }
        if (op==TypePointer && n==4 && w[i+1]<bound && w[i+2]==Input) pointee[w[i+1]]=w[i+3];
        if (op==Variable && n>=4 && w[i+3]==PushConstant) existing_push=1;
        if (op==Decorate && n==4 && w[i+1]<bound && w[i+2]==BuiltIn) {
            uint32_t builtin=w[i+3];
            // Tag 4 keeps the native input and adds the staged vertex bias.
            if (builtin==42 || (native && builtin==4424)) member[w[i+1]]=4;
            else if (!native && builtin>=4424 && builtin<=4426) member[w[i+1]]=(uint8_t)(builtin-4424+1);
            if (member[w[i+1]]) count++;
        }
        i+=n;
    }
    if ((!cap || native) && !count) { *ok=1; goto fail; }
    if (!first_fn || !first_type || (count && (existing_push || entries!=1 || !vertex))) goto fail;
    int variables=0; size_t loads=0;
    for (size_t i=5;i<nw;i+=w[i]>>16) {
        uint32_t op=w[i]&0xffff,n=w[i]>>16;
        if (op==Variable && n>=4 && w[i+2]<w[3] && member[w[i+2]]) {
            if (n!=4 || w[i+3]!=Input || w[i+1]>=w[3] || pointee[w[i+1]]>=w[3] || !integer[pointee[w[i+1]]]) goto fail;
            variables++;
        }
        if (op==Load && n>=4 && w[i+3]<w[3] && member[w[i+3]]) {
            if (n!=4 || w[i+1]>=w[3] || !integer[w[i+1]]) goto fail;
            loads++;
        }
    }
    if (variables!=count) goto fail;
    // A signed VertexIndex load gains a push load, cast and add (18 words).
    uint32_t *o=malloc((nw+18*loads+128)*sizeof *o);
    if (!o) goto fail;
    uint32_t st=0, ps=0, pu=0, push=0, index[4]={0};
    int new_u32=!u32;
    if (count) {
        if (!u32) u32=bound++;
        st=bound++; ps=bound++; pu=bound++; push=bound++;
        for (int j=0;j<4;j++) index[j]=bound++;
    }
    size_t at=5; memcpy(o,w,5*sizeof *w);
    for (size_t i=5;i<nw;i+=w[i]>>16) {
        uint32_t op=w[i]&0xffff,n=w[i]>>16;
        if (count && i==first_type) {
            const uint32_t decl[]={3u<<16|Decorate,st,Block,
                5u<<16|MemberDecorate,st,0,Offset,0,
                5u<<16|MemberDecorate,st,1,Offset,4,
                5u<<16|MemberDecorate,st,2,Offset,8,5u<<16|MemberDecorate,st,3,Offset,12};
            memcpy(o+at,decl,sizeof decl);at+=sizeof decl/4;
        }
        if (count && i==first_fn) {
            if (new_u32) { const uint32_t d[]={4u<<16|TypeInt,u32,32,0};memcpy(o+at,d,sizeof d);at+=4; }
            const uint32_t decl[]={6u<<16|TypeStruct,st,u32,u32,u32,u32,
                4u<<16|TypePointer,ps,PushConstant,st,
                4u<<16|TypePointer,pu,PushConstant,u32,
                4u<<16|Variable,ps,push,PushConstant};
            memcpy(o+at,decl,sizeof decl);at+=sizeof decl/4;
            for (int j=0;j<4;j++) { const uint32_t d[]={4u<<16|Constant,u32,index[j],(uint32_t)j};memcpy(o+at,d,sizeof d);at+=4; }
        }
        if (!native && op==Capability && n==2 && w[i+1]==DrawParameters) continue;
        if (!native && op==Extension && n>1 && memchr(w+i+1,0,(n-1)*4) && !strcmp((const char *)(w+i+1),"SPV_KHR_shader_draw_parameters")) continue;
        if ((op==Name || op==Decorate) && n>=3 && w[i+1]<w[3] && member[w[i+1]] && member[w[i+1]]!=4) continue;
        if (op==Variable && n>=4 && w[i+2]<w[3] && member[w[i+2]] && member[w[i+2]]!=4) continue;
        if (count && op==EntryPoint) {
            size_t args=3; // skip the nul-terminated entry-point name
            while (args<n && !memchr(w+i+args,0,4)) args++;
            if (args==n) { free(o);goto fail; }
            args++;
            size_t start=at;memcpy(o+at,w+i,args*4);at+=args;
            for (size_t j=args;j<n;j++) if (w[i+j]>=w[3] || !member[w[i+j]] || member[w[i+j]]==4) o[at++]=w[i+j];
            // SPIR-V 1.4+ lists all global interface variables, including push constants.
            if (w[1]>=0x00010400) o[at++]=push;
            o[start]=(uint32_t)(at-start)<<16|EntryPoint;
            continue;
        }
        if (op==Load && n==4 && w[i+3]<w[3] && member[w[i+3]]) {
            int adjust=member[w[i+3]]==4;
            uint32_t ptr=bound++, dest=adjust ? bound++ : w[i+2];
            uint32_t result=w[i+1]==u32 ? dest : bound++;
            const uint32_t load[]={5u<<16|AccessChain,pu,ptr,push,index[member[w[i+3]]-1],4u<<16|Load,u32,result,ptr};
            memcpy(o+at,load,sizeof load);at+=sizeof load/4;
            if (w[i+1]!=u32) { const uint32_t cast[]={4u<<16|Bitcast,w[i+1],dest,result};memcpy(o+at,cast,sizeof cast);at+=4; }
            if (adjust) {
                uint32_t raw=bound++;
                const uint32_t add[]={4u<<16|Load,w[i+1],raw,w[i+3],5u<<16|IAdd,w[i+1],w[i+2],raw,dest};
                memcpy(o+at,add,sizeof add);at+=9;
            }
            continue;
        }
        memcpy(o+at,w+i,n*4);at+=n;
    }
    o[3]=bound;*out_n=at;*ok=1;
    free(member);free(integer);free(pointee);return o;
fail:
    free(member);free(integer);free(pointee);return NULL;
}
