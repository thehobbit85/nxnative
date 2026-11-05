// Quad-varying offsets, continuous coordinates and an independent nearest oracle.
#ifndef DM_FRAGMENT_TEXTURE_CASES_H
#define DM_FRAGMENT_TEXTURE_CASES_H
#include "texture_ops_matrix.h"
enum { SIDE = 16, FRAGMENT_CASES = 40 };
static const float fragment_scales[] = {.03125f,.0625f,.125f,.25f,.5f};
static const float fragment_biases[] = {-1.25f,0,.375f,1.375f};
static void fragment_inputs(unsigned index, uint32_t *uniform) {
    memset(uniform,0,65536);
    uniform[0]=uniform[1]=tex_bits(fragment_scales[index/8]);
    uniform[2]=tex_bits(fragment_biases[(index/2)%4]);
}
static int fragment_check(unsigned variant,unsigned mode,unsigned index,const uint32_t *pixels) {
    for(unsigned y=0;y<SIDE;y++)for(unsigned x=0;x<SIDE;x++)for(unsigned c=0;c<4;c++) {
        unsigned word=(y*SIDE+x)*4+c;float value;memcpy(&value,&pixels[word],4);
        if(!isfinite(value) || value<0 || value>50000) {
            fprintf(stderr,"Fragment variant%u mode%u case%u invalid word%u: %g\n",variant,mode,index,word,value);return 1;
        }
        if(!mode) {
            float scale=fragment_scales[index/8],lod=log2f(8*scale)+(variant?fragment_biases[(index/2)%4]:0);
            unsigned mip=(unsigned)fminf(3,fmaxf(0,floorf(lod+.5f))),size=8u>>mip;
            int ox=x<8?(int)x:(int)x-16,oy=y<8?(int)y:(int)y-16;
            int tx=tex_address((int)floorf(((float)x+.5f)*scale*(float)size)+ox,(int)size,index%2);
            int ty=tex_address((int)floorf(((float)y+.5f)*scale*(float)size)+oy,(int)size,index%2);
            float expected=tex_value(mip,(unsigned)tx,(unsigned)ty,c);
            if(value!=expected) {
                fprintf(stderr,"Fragment variant%u case%u pixel(%u,%u).%u: %g expected %g\n",variant,index,x,y,c,value,expected);return 1;
            }
        }
    }
    return fwrite(pixels,16,SIDE*SIDE,stdout)==SIDE*SIDE?0:1;
}
#endif
