#version 450
void main() {
    const vec2 p[3]=vec2[3](vec2(-.6875,-.6875),vec2(.6875,-.6875),vec2(0,.6875));
    gl_Position=vec4(p[gl_VertexIndex],.25,1);
}
