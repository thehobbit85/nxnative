// Shared pool words and channel expectations for the Metal and Vulkan tests.
// The widths and shifts below describe the guest's little-endian layout.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static const uint16_t packed_texel_words[]={0x8c31,0x402f,0x03c0,0xf012};
static const struct packed_texel_case {
    NVNformat format;
    uint8_t bits[4],shift[4];
} packed_texels[]={
    {NVN_FORMAT_RGBA4, {4,4,4,4},{0,4,8,12}},
    {NVN_FORMAT_RGB5,  {5,5,5,0},{0,5,10,0}},
    {NVN_FORMAT_RGB5A1,{5,5,5,1},{0,5,10,15}},
    {NVN_FORMAT_RGB565,{5,6,5,0},{0,5,11,0}},
    {NVN_FORMAT_BGR565,{5,6,5,0},{11,5,0,0}},
    {NVN_FORMAT_BGR5,  {5,5,5,0},{10,5,0,0}},
    {NVN_FORMAT_BGR5A1,{5,5,5,1},{10,5,0,15}},
    {NVN_FORMAT_A1BGR5,{5,5,5,1},{11,6,1,0}},
};
static int packed_texel_matches(const struct packed_texel_case *fmt,int swizzled,const float values[4][4]) {
    // The override selects B, alpha, R and zero from the formatted texel.
    for (int i=0;i<4;i++) for (int c=0;c<4;c++) {
        int component=swizzled ? ((int[]){2,3,0,-1})[c] : c;
        float want=0;
        if (component>=0) {
            uint32_t mask=(1u<<fmt->bits[component])-1;
            want=mask ? (float)((packed_texel_words[i]>>fmt->shift[component])&mask)/mask : 1.0f;
        }
        // Metal's fixed-function UNORM unpack can round through half precision.
        if (!(fabsf(values[i][c]-want)<0.0006f)) {
            fprintf(stderr,"packed format %#x view %d word %#x channel %d: %g expected %g\n",
                    fmt->format,swizzled,packed_texel_words[i],c,values[i][c],want);
            return 0;
        }
    }
    return 1;
}
