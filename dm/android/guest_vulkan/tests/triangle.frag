#version 450
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform Colors { vec4 value; } colors;
void main() { color = colors.value; }
