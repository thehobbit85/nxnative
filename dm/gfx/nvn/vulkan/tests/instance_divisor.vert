#version 450
#extension GL_ARB_shader_draw_parameters : require
layout(location=0) in float red;
layout(location=1) in float green;
layout(location=0) flat out vec2 tint;
void main() {
    const vec2 corners[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
    vec2 p = corners[gl_VertexIndex];
    gl_Position = vec4(-1 + 2*(gl_InstanceIndex-4+p.x)/7, 2*p.y-1, 0, 1);
    tint = vec2(gl_BaseInstanceARB==5 ? red : 0,green);
}
