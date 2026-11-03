#version 450
layout(location = 0) in vec4 floats;
layout(location = 1) in uvec4 unsigneds;
layout(location = 2) in ivec4 signeds;
layout(set = 0, binding = 0) uniform Check { uvec4 expected; uint location; } check;
layout(location = 0) flat out uint match;
void main() {
    uvec4 bits = check.location == 0 ? floatBitsToUint(floats) :
                 check.location == 1 ? unsigneds : uvec4(signeds);
    match = uint(all(equal(bits, check.expected)));
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(2 * p - 1, 0, 1);
}
