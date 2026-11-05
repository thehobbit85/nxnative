#version 450
layout(location=0) flat in vec4 color;
layout(location=1) flat in uint base_instance;
layout(set=0,binding=2,std140) uniform Driver { uvec4 rows[256]; } c0;
layout(location=0) out vec4 result;
void main() {
    bool ok=all(equal(c0.rows[0x620/16],uvec4(0x88,0,0,0))) &&
            all(equal(c0.rows[0x630/16],uvec4(0))) &&
            all(equal(c0.rows[0x640/16],uvec4(0,base_instance,0,0))) &&
            uintBitsToFloat(c0.rows[0x610/16].x)==2047;
    result=ok ? color : vec4(1,0,1,1);
}
