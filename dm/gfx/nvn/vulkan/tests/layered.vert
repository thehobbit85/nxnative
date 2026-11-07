#version 450
#extension GL_ARB_shader_viewport_layer_array : require
layout(location=0) flat out uint value;
void main() {
    vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    gl_Position=vec4(2*p-1,.25,1);
#ifndef SINGLE_LAYER
    gl_Layer=gl_InstanceIndex;
#endif
    value=uint(gl_InstanceIndex)+100u;
}
