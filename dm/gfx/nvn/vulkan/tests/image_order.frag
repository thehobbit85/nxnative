#version 450
layout(binding=0, r32ui) uniform uimage2D data;
layout(location=0) out uint value;
void main() {
    value = imageAtomicAdd(data, ivec2(gl_FragCoord.xy), 3u);
}
