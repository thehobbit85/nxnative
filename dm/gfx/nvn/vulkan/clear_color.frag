#version 450
layout(push_constant) uniform ClearValue { COLOR_TYPE rgba; } value;
layout(location = 0) out COLOR_TYPE color;
void main() { color = value.rgba; }
