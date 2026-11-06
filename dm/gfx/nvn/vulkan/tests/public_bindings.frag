#version 450
layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(location = 0) out vec4 color;
void main() {
    color = texture(sourceImage, vec2(gl_FragCoord.x * 0.5 + 0.125, 0.5));
}
