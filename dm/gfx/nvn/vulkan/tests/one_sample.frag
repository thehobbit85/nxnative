#version 450
layout(location=0) out vec4 color;
void main() {
    gl_SampleMask[0] = (int(gl_FragCoord.x) & 1) == 0 ? 1 : 2;
    color = vec4(1, 0, 0, 1);
}
