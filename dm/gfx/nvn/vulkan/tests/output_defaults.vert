#version 450
layout(location = 24) out vec4 world;
layout(location = 27) out vec4 other;
void main() {
    const vec2 positions[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(positions[gl_VertexIndex], 0, 1);
    world.xyz = vec3(0.25, 0.5, 0.75); // The fragment shader consumes unwritten W.
    other.y = 0.5;
    other.w = 0.25; // A written W must override the default.
}
