#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set=0, binding=0) uniform texture2D sourceImage;
layout(location=0) out vec4 color;
layout(push_constant) uniform Blit {
    ivec4 source;       // origin and signed extent, including the mirror flags
    ivec4 destination;
    int linear;
} blit;

void main() {
    vec2 unit = (gl_FragCoord.xy - vec2(blit.destination.xy)) / vec2(blit.destination.zw);
    vec2 p = vec2(blit.source.xy) + unit * vec2(blit.source.zw);
    ivec2 hi = textureSize(sourceImage,0) - 1;
    if (blit.linear == 0) {
        color = texelFetch(sourceImage,clamp(ivec2(floor(p)),ivec2(0),hi),0);
        return;
    }
    // Linear sampling can also be unavailable for an otherwise sampleable
    // format. Fetch four texels and interpolate in the source's numeric space.
    vec2 centered = p - 0.5;
    ivec2 base = ivec2(floor(centered));
    ivec2 a = clamp(base,ivec2(0),hi), b = clamp(base+1,ivec2(0),hi);
    vec2 f = fract(centered);
    vec4 top = mix(texelFetch(sourceImage,a,0),texelFetch(sourceImage,ivec2(b.x,a.y),0),f.x);
    vec4 bottom = mix(texelFetch(sourceImage,ivec2(a.x,b.y),0),texelFetch(sourceImage,b,0),f.x);
    color = mix(top,bottom,f.y);
}
