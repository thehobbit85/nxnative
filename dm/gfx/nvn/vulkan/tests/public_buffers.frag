#version 450
layout(binding=0, std140) uniform Constants { uvec4 value; } c;
layout(binding=1, std430) buffer Storage { uint words[]; } data;
layout(location=0) flat in uvec2 vertex_value;
layout(location=0) out uvec4 color;
void main() {
    color = uvec4(vertex_value, c.value.xy);
    atomicAdd(data.words[0], c.value.x);
    atomicAdd(data.words[1], c.value.y);
}
