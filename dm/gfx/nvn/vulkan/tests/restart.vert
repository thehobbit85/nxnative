#version 450
void main() {
    const vec2 p[]=vec2[](
        vec2(-.9,-.8),vec2(-.3,-.8),vec2(-.3,.8),vec2(-.9,.8),
        vec2(.3,-.8),vec2(.9,-.8),vec2(.9,.8),vec2(.3,.8));
    gl_Position=vec4(p[uint(gl_VertexIndex)&7u],0.5,1);
}
