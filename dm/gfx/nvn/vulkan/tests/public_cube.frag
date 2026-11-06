#version 450
layout(set = 0, binding = 0) uniform samplerCube firstCube;
layout(set = 0, binding = 1) uniform samplerCube secondCube;
layout(location = 0) out vec4 color;
void main() {
    const vec3 faces[] = vec3[](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0),
                                vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
    vec3 direction = faces[int(gl_FragCoord.x)];
    color = vec4(texture(firstCube, direction).r, texture(secondCube, direction).r, 0, 1);
}
