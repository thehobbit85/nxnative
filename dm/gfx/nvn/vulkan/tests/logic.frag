#version 450
layout(set=0,binding=0) uniform Words { uvec4 src; } words;
#if MIXED_TARGET
layout(location=0) out vec4 color;
layout(location=1) out vec4 raw;
void main() {
    color=vec4(1,0,0,(int(gl_FragCoord.x)&1)!=0 ? .75 : .25);
    raw.x=uintBitsToFloat(words.src.x);
    raw.y=uintBitsToFloat(words.src.y);
    raw.z=raw.x;
    raw.w=uintBitsToFloat(words.src.w);
    if ((int(gl_FragCoord.x)&1)!=0) raw.z=uintBitsToFloat(words.src.z);
}
#elif UINT_TARGET
layout(location=0) out uvec4 color;
void main() { color=words.src; }
#else
layout(location=0) out vec4 color;
void main() { color=uintBitsToFloat(words.src); }
#endif
