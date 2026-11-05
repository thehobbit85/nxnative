#version 450
#if SEPARATE
layout(set=0,binding=0) uniform texture3D image;
layout(set=0,binding=1) uniform sampler sampling;
#else
layout(set=0,binding=0) uniform sampler3D image;
#endif
layout(set=0,binding=2) uniform Parameters { float coordinate; } p;
layout(location=0) out vec4 color;
void main() {
#if SEPARATE
    color = texture(sampler3D(image, sampling), vec3(p.coordinate, 0.5, 0.5));
#else
    color = texture(image, vec3(p.coordinate, 0.5, 0.5));
#endif
}
