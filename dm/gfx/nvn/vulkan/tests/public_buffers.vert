#version 450
layout(binding=0, std140) uniform Constants { uvec4 value; } c;
layout(binding=1, std430) buffer Storage { uint words[]; } data;
layout(location=0) flat out uvec2 vertex_value;
void main() {
    const vec2 corners[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(corners[gl_VertexIndex], 0, 1);
    vertex_value = c.value.xy;
    if (gl_VertexIndex == 0) {
        atomicAdd(data.words[0], c.value.x);
        atomicAdd(data.words[1], c.value.y);
    }
}
