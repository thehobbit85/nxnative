#version 450
layout(set=0, binding=0) uniform sampler2D depthImage;
layout(location=0) out vec4 color;
void main() { color = vec4(texture(depthImage, gl_FragCoord.xy / 8.0).r, 0, 0, 1); }
