#version 450
#extension GL_ARB_shader_draw_parameters : require
layout(set=0,binding=1,std140) uniform Expected { ivec4 value; } expected;
layout(set=0,binding=2,std140) uniform Driver { uvec4 rows[256]; } c0;
layout(location=0) flat out vec4 color;
layout(location=1) flat out uint base_instance;
void main() {
    const vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    int vertex=gl_VertexIndex-expected.value.z;
    gl_Position=vec4(positions[clamp(vertex,0,2)],0,1);
    int instance=gl_InstanceIndex-gl_BaseInstanceARB;
    base_instance=uint(expected.value.x);
    bool ok=c0.rows[0x640/16].y==base_instance && c0.rows[0x640/16].x==0 &&
            uintBitsToFloat(c0.rows[0x610/16].x)==2047 &&
            gl_BaseInstanceARB==expected.value.x && gl_BaseVertexARB==expected.value.y &&
            gl_DrawIDARB==0 && instance>=0 && instance<2 && vertex>=0 && vertex<3;
    color=ok ? vec4(0,1,0,1) : vec4(1,0,0,1);
}
