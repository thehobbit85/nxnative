// Adapted from Snapdragon Game Super Resolution 2's sgsr2_convert.fs.
// Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause. See dm/third_party/sgsr2/LICENSE.
#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set=0,binding=0) uniform texture2D depthImage;
layout(set=0,binding=1) uniform texture2D motionImage;
layout(set=0,binding=2) uniform texture2D colorImage;
layout(set=0,binding=3) uniform texture2D exposureImage;
layout(push_constant) uniform Prepare {
    ivec2 size, depthBase, motionBase, colorBase;
    vec2 motionScale;
    int depthReversed, exposureMode;   // 0 unity, 1 guest texture, 2 auto meter
    float preExposure;
} p;
layout(location=0) out vec4 metadata;

float depthAt(ivec2 pos) {
    float d=texelFetch(depthImage,p.depthBase+clamp(pos,ivec2(0),p.size-1),0).r;
    return p.depthReversed!=0 ? 1.0-d : d;
}
float nearestInQuad(ivec2 pos) {
    return min(min(depthAt(pos),depthAt(pos+ivec2(1,0))),min(depthAt(pos+ivec2(0,1)),depthAt(pos+1)));
}
float frameExposure() {
    if (p.exposureMode==1) return max(0.0001,texelFetch(exposureImage,ivec2(0),0).r);
    if (p.exposureMode!=2) return 1.0;
    // Only texel (0,0) computes this frame's exposure from a stratified grid.
    float logSum=0.0;
    for (int y=0;y<16;y++) for (int x=0;x<16;x++) {
        ivec2 pos=ivec2((vec2(x,y)+0.5)*vec2(p.size)/16.0);
        vec3 rgb=texelFetch(colorImage,p.colorBase+pos,0).rgb/p.preExposure;
        logSum+=log2(max(0.0001,dot(rgb,vec3(0.2126,0.7152,0.0722))));
    }
    return 0.18*exp2(-logSum/256.0);
}
void main() {
    ivec2 pos=ivec2(gl_FragCoord.xy);
    float center=nearestInQuad(pos);
    float depthClip=0.0;
    if (center<0.99999) {
        // NVN supplies screen-space motion directly, without a projection
        // matrix or FOV. Use SGSR's separation at unit horizontal FOV scale.
        float separation=1.37e-5*length(vec2(p.size))*(1.0-center);
        float weight=0.0;
        for (int y=-1;y<=1;y+=2) for (int x=-1;x<=1;x+=2)
            weight+=min(1.0,separation/(abs(center-nearestInQuad(pos+ivec2(x,y)))+1.19e-7));
        depthClip=1.0-0.25*weight;
    }
    vec2 motion=texelFetch(motionImage,p.motionBase+pos,0).xy*p.motionScale/vec2(p.size);
    metadata=vec4(motion,depthClip,all(equal(pos,ivec2(0))) ? frameExposure() : 0.0);
}
