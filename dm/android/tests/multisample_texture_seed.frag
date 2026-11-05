#version 450
layout(push_constant) uniform Seed { uint layer; } seed;
layout(location=0) out vec4 color;
void main() {
    float value=float(10000u*seed.layer+1000u*uint(gl_SampleID)+100u*uint(gl_FragCoord.y)+uint(gl_FragCoord.x)+1u);
    color=vec4(value,value+100000.0,value+200000.0,value+300000.0);
}
