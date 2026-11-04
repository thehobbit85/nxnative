#version 450
layout(set = 1, binding = 0) uniform sampler3D images[32];
layout(set = 1, binding = 1) uniform Parameters { uint index; float coordinate; } p;
layout(location = 0) out vec4 color;
void main() { color = texture(images[p.index], vec3(p.coordinate, 0.5, 0.5)); }
