#version 450
layout(set=1,binding=0,r32ui) coherent uniform uimageBuffer counters;
layout(set=1,binding=1) uniform usamplerBuffer source;
layout(location=0) out vec4 color;
void main() {
    int i=int(gl_FragCoord.x)+8*int(gl_FragCoord.y);
    imageAtomicAdd(counters,i,2);
    color=vec4(float(texelFetch(source,0).r)/255.0,0,0,1);
}
