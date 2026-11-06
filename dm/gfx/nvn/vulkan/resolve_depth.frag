#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set=0, binding=0) uniform texture2DMS sourceDepth;
layout(push_constant) uniform Resolve { ivec2 sourceOffset; } region;
void main() {
    gl_FragDepth = texelFetch(sourceDepth, ivec2(gl_FragCoord.xy) + region.sourceOffset, 0).r;
}
