// Adapted from Snapdragon Game Super Resolution 2's sgsr2_upscale.fs.
// Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause. See dm/third_party/sgsr2/LICENSE.
#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set=0,binding=0) uniform texture2D currentImage;
layout(set=0,binding=1) uniform texture2D metadata;
layout(set=0,binding=2) uniform texture2D historyImage;
layout(set=0,binding=3) uniform texture2D depthImage;
layout(push_constant) uniform Resolve {
    ivec2 inputSize, outputSize, colorBase;
    vec2 jitter;
    float preExposure, previousPreExposure;
    int reset, depthReversed;
    ivec2 depthBase;
} p;
layout(location=0) out vec4 reconstructed;

// Perform clipping/blending in bounded HDR space. Return the guest's original
// pre-exposed linear units to the output and store those same units in history.
vec3 compressColor(vec3 rgb) { return rgb/(1.0+abs(rgb)); }
vec3 linearColor(vec3 rgb) { return rgb/max(vec3(1e-7),1.0-abs(rgb)); }
// Store the input depth in history alpha. Reject taps from another surface
// before the bilinear blend, especially a foreground that just moved away.
vec4 sampleHistory(vec2 uv,float depth) {
    vec2 center=uv*vec2(p.outputSize)-0.5;
    ivec2 base=ivec2(floor(center));
    vec2 f=fract(center);
    float nearDepth=p.depthReversed!=0 ? depth : 1.0-depth;
    float tolerance=max(0.0002,0.05*nearDepth);
    vec4 sum=vec4(0);
    for (int y=0;y<2;y++) for (int x=0;x<2;x++) {
        vec4 tap=texelFetch(historyImage,clamp(base+ivec2(x,y),ivec2(0),p.outputSize-1),0);
        float w=(y==1 ? f.y : 1.0-f.y)*(x==1 ? f.x : 1.0-f.x);
        if (abs(tap.a-depth)<=tolerance) sum+=vec4(tap.rgb,1.0)*w;
    }
    return sum;
}
void main() {
    vec2 uv=gl_FragCoord.xy/vec2(p.outputSize);
    vec2 inputPos=uv*vec2(p.inputSize)+p.jitter;
    ivec2 pixel=clamp(ivec2(floor(inputPos)),ivec2(0),p.inputSize-1);
    vec3 mda=texelFetch(metadata,pixel,0).xyz;
    vec2 prevUV=uv+mda.xy;     // NVN motion points from current to previous
    float speed=2.0*length(mda.xy*vec2(p.outputSize));
    vec2 ratio=vec2(p.outputSize)/vec2(p.inputSize);
    float bias=mix(ratio.x,max(1.0,0.3+0.3*ratio.x),0.25*mda.z)*0.5;
    float curve=mix(-2.0,-3.0,clamp(speed*0.02,0.0,1.0));
    float exposure=texelFetch(metadata,ivec2(0),0).a/p.preExposure;
    vec3 lo=vec3(1), hi=vec3(-1), average=vec3(0), square=vec3(0), sum=vec3(0);
    float weight=0.0, boxWeight=0.0;
    // SGSR2's performance preset uses the center and its four axial neighbours.
    const ivec2 taps[5]=ivec2[](ivec2(0,0),ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
    for (int i=0;i<5;i++) {
        ivec2 coord=pixel+taps[i];
        vec4 texel=texelFetch(currentImage,p.colorBase+clamp(coord,ivec2(0),p.inputSize-1),0);
        vec3 sampleColor=compressColor(texel.rgb*exposure);
        lo=min(lo,sampleColor); hi=max(hi,sampleColor);
        vec2 offset=vec2(coord)+0.5-inputPos;
        float distance2=dot(offset,offset);
        float y=clamp(distance2*bias*bias,0.0,1.0)-1.0, y2=y*y;
        float lanczos=(0.75*y+y2)*y2;
        sum+=sampleColor*lanczos; weight+=lanczos;
        float w=exp(curve*distance2);
        average+=w*sampleColor; square+=w*sampleColor*sampleColor; boxWeight+=w;
    }
    vec3 current=weight>1e-7 ? clamp(sum/weight,lo,hi) : average/max(boxWeight,1e-7);
    float depth=texelFetch(depthImage,p.depthBase+pixel,0).r;
    vec4 previous=vec4(0.0);
    if (p.reset==0 && all(greaterThanEqual(prevUV,vec2(0))) && all(lessThanEqual(prevUV,vec2(1)))) previous=sampleHistory(prevUV,depth);
    if (previous.w>1e-7) {
        vec3 history=compressColor(previous.rgb/previous.w*(exposure*p.preExposure/p.previousPreExposure));
        average/=max(boxWeight,1e-7);
        vec3 deviation=sqrt(abs(square/max(boxWeight,1e-7)-average*average));
        float boxSize=mix(min(20.0,pow(ratio.x*ratio.y,3.0)),1.0,max(mda.z,clamp(speed*0.05,0.0,1.0)));
        lo=max(lo,average-boxSize*deviation); hi=min(hi,average+boxSize*deviation);
        vec3 clipped=clamp(history,lo,hi);
        float wc=max(weight/3.0,0.0), wh=1.0-mda.z;
        wh=min(wh,mix(wh,10.0*wc,clamp(10.0*speed,0.0,1.0)));
        wh=min(wh,mix(wh,wc,clamp(speed*0.05,0.0,1.0)));
        if (any(notEqual(history,clipped))) wh=min(wh,0.1);
        current=mix(clipped,current,wh+wc>1e-7 ? wc/(wh+wc) : 1.0);
    }
    reconstructed=vec4(linearColor(current)/exposure,depth);
}
