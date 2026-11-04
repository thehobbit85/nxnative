#version 450
layout(set=0,binding=0) uniform texture3D images[2];
layout(set=0,binding=1) uniform sampler samplers[2];
layout(set=0,binding=2) uniform Parameters { uint image_index; uint sampler_index; float coordinate; } p;
layout(location=0) out vec4 color;
void main() {
    color=texture(sampler3D(images[p.image_index],samplers[p.sampler_index]),vec3(p.coordinate,0.5,0.5));
}
