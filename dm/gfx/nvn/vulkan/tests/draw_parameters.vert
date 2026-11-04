#version 450
#extension GL_ARB_shader_draw_parameters : require
layout(set=0,binding=1,std140) uniform Expected { ivec4 value; } expected;
layout(location=0) flat out vec4 color;
void main() {
    const vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(positions[gl_VertexIndex],0,1);
    int instance=gl_InstanceIndex-gl_BaseInstanceARB;
    bool ok=gl_BaseInstanceARB==expected.value.x && gl_BaseVertexARB==expected.value.y &&
            gl_DrawIDARB==0 && instance>=0 && instance<2;
    color=ok ? vec4(0,1,0,1) : vec4(1,0,0,1);
}
