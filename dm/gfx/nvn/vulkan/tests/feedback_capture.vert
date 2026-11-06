#version 450
layout(location=0) out uvec4 id_bits;
layout(location=3) out vec2 position_bits;
layout(set=0,binding=0,std430) buffer Counter { uint vertices; } counter;
void main() {
    uint v = uint(gl_VertexIndex), i = uint(gl_InstanceIndex);
    id_bits = uvec4(v, i, 0xf1230000u | v, 1);
    position_bits = vec2(float(v), -float(i));
    atomicAdd(counter.vertices, 1);
    uint corner = v % 3;
    vec2 p = vec2((corner << 1) & 2, corner & 2);
    gl_Position = vec4(2*p-1, 0.5, 1);
}
