#ifndef DM_FRAGMENT_LOD_QUERY_CASES_H
#define DM_FRAGMENT_LOD_QUERY_CASES_H
#include "texture_ops_matrix.h"
enum { SIDE=16,FRAGMENT_CASES=98 };
static const float lod_scales[]={.015625f,.03125f,.0625f,.125f,.25f,.5f,1};
static tex_config_t lod_config(unsigned mode) {
    tex_config_t config=tex_config(mode==6?0:mode);
    if(mode==6) { config.min_lod=1.25f;config.max_lod=2.25f; }
    return config;
}
static void fragment_inputs(unsigned mode,unsigned index,uint32_t *uniform) {
    (void)mode;memset(uniform,0,65536);uniform[0]=tex_bits(lod_scales[index/2%7]);uniform[1]=tex_bits(lod_scales[index/14%7]);
}
static int fragment_check(unsigned variant,unsigned mode,unsigned index,const uint32_t *pixels) {
    tex_config_t config=lod_config(mode);
    float x=lod_scales[index/2%7]*tex_dimension(config.width,config.base),y=lod_scales[index/14%7]*tex_dimension(config.height,config.base);
    float lod=log2f(fmaxf(x,y));float clamped=fminf(config.max_lod,fmaxf(config.min_lod,lod));
    clamped=fminf((float)(config.levels-config.base-1),fmaxf(0,clamped));
    if(!config.mip_linear)clamped=floorf(clamped+.5f);
    float expected[]={(float)(int32_t)(lod*256),(float)(int32_t)(clamped*256),1234,5678};
    for(unsigned word=0;word<SIDE*SIDE*4;word++) {
        uint32_t want=tex_bits(expected[word%4]);
        if(pixels[word]!=want) { float value;memcpy(&value,&pixels[word],4);fprintf(stderr,"LOD query variant%u mode%u case%u word%u: %g expected %g\n",variant,mode,index,word,value,expected[word%4]);return 1; }
    }
    return fwrite(pixels,16,SIDE*SIDE,stdout)==SIDE*SIDE?0:1;
}
#endif
