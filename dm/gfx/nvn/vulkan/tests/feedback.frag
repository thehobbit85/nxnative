#version 450
layout(binding=0) uniform sampler2D first;
layout(binding=1) uniform sampler2D second;
layout(location=0) out vec4 left;
layout(location=2) out vec4 right;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 a = texelFetch(first, p, 0).rg;
    vec2 b = texelFetch(second, p, 0).rg;
    left = vec4(b.r, a, 1);
    right = vec4(a.g, b, 1);
}
