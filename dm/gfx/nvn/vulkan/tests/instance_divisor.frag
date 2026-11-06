#version 450
layout(location=0) flat in vec2 tint;
layout(location=0) out vec4 color;
void main() { color=vec4(tint,0,1); }
