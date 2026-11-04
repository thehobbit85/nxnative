#version 450
layout(set=0,binding=0,r32ui) coherent uniform uimageBuffer counters;
layout(set=0,binding=1) uniform usamplerBuffer source;
void main() {
    vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    imageAtomicAdd(counters,gl_VertexIndex,1);
    gl_Position=vec4(positions[gl_VertexIndex],0,texelFetch(source,0).r>0?1:0);
}
