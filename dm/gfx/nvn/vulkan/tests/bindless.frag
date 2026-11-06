#version 450
#ifdef HANDLE_TABLE
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 1, binding = 0) uniform sampler3D images[];
layout(set = 1, binding = 2, std430) readonly buffer SlotMap { uint n; uint slot[]; } table;
#define SLOT(i) nonuniformEXT(table.slot[min(i, table.n)])
#else
layout(set = 1, binding = 0) uniform sampler3D images[32];
#define SLOT(i) (i)
#endif
layout(set = 1, binding = 1) uniform Parameters { uint index; float coordinate; } p;
layout(location = 0) out vec4 color;
void main() { color = texture(images[SLOT(p.index)], vec3(p.coordinate, 0.5, 0.5)); }
