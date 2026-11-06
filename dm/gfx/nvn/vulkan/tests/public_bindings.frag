#version 450
#ifdef SEPARATE
layout(set = 0, binding = 0) uniform texture2D image;
layout(set = 0, binding = 1) uniform sampler filterState;
#define sourceImage sampler2D(image, filterState)
#else
layout(set = 0, binding = 0) uniform sampler2D sourceImage;
#endif
layout(location = 0) out vec4 color;
void main() {
    color = texture(sourceImage, vec2(gl_FragCoord.x * 0.5 + 0.125, 0.5));
}
