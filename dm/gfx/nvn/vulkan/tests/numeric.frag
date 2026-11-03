#version 450
layout(location = 0) flat in uint match;
layout(location = 0) out vec4 color;
void main() { color = match != 0 ? vec4(0, 1, 0, 1) : vec4(1, 0, 0, 1); }
