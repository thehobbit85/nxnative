#version 450
layout(location = 24) in vec4 world;
layout(location = 27) in vec4 other;
layout(location = 0) out vec4 color;
void main() {
    bool ok = all(equal(world.xyz / world.w, vec3(0.25, 0.5, 0.75))) &&
              all(equal(other, vec4(0, 0.5, 0, 0.25)));
    color = ok ? vec4(0,1,0,1) : vec4(1,0,0,1);
}
