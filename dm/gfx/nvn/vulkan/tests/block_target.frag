#version 450
layout(binding=0) uniform Block { uvec4 words; } encoded;
layout(location=0) out uvec4 block;
void main() { block=encoded.words; }
