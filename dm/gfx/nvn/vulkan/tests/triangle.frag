#version 450
layout(set = 0, binding = 0) uniform samplerCube faces;
layout(set = 0, binding = 1) uniform Color { vec4 color; } u;
layout(set = 0, binding = 2, std140) uniform GraphicsPush { layout(offset = 240) vec4 tint; } gfx;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(faces, vec3(1, 0, 0)) * u.color * gfx.tint; }
