#version 450
layout(set=0,binding=0,std430) buffer Fragments { uint words[]; } fragments;
layout(set=0,binding=1,std140) uniform Push { uvec4 rows[16]; } constants;
layout(location=0) out vec4 color;
void main() {
    uint pixel=uint(gl_FragCoord.y)*8u+uint(gl_FragCoord.x);
    uint old=atomicAdd(fragments.words[pixel],1);
    color=old==constants.rows[15].x?vec4(0,1,0,1):vec4(1,0,0,1);
}
