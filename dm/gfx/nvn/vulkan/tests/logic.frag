#version 450
layout(set=0,binding=0) uniform Words { uvec4 src; } words;
#if UINT_TARGET
layout(location=0) out uvec4 color;
void main() { color=words.src; }
#else
layout(location=0) out vec4 color;
void main() { color=uintBitsToFloat(words.src); }
#endif
