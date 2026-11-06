#version 450
layout(location=27) in vec4 normalized;
layout(location=31) in ivec4 integer;
layout(set=0,binding=0) uniform Expected { ivec4 value; } expected;
layout(location=0) flat out uint match;
void main() {
    int vertex=gl_VertexIndex-4;
    const vec2 corners[]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    bool ok=all(equal(ivec4(round(normalized*vec4(1023,1023,1023,3))),ivec4(374,491,857,2))) &&
            all(equal(integer,expected.value)) && vertex>=0 && vertex<3;
    match=uint(ok);
    gl_Position=ok ? vec4(corners[vertex],0,1) : vec4(2,2,0,1);
}
