#version 450
layout(set=0,binding=0,std430) buffer Vertices { uint words[]; } vertices;
void main() {
    const vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(p[gl_VertexIndex],0,1);
    atomicAdd(vertices.words[gl_VertexIndex],1);
}
