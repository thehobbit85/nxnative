#version 450
layout(binding=0) uniform sampler1D lines[2];
layout(binding=1) uniform sampler2D rows[2];
layout(binding=2) uniform sampler1DShadow shadowLine;
layout(binding=3) uniform sampler1DArrayShadow shadowArray;
layout(location=0) in vec2 coordinate;
layout(location=0) out vec4 color;
vec4 sampleLine(sampler1D image, float x) { return texture(image, x); }
vec4 sampleRow(sampler2D image, vec2 xy) { return texture(image, xy); }
void main() {
    int index = int(coordinate.y) & 1;
    color = sampleLine(lines[index], coordinate.x) + sampleRow(rows[index], coordinate);
    color += texture(lines[0], coordinate.x, 0.5);
    color += textureOffset(lines[0], coordinate.x, 1);
    color += textureProj(lines[0], vec2(coordinate.x, 2));
    color += textureProjGradOffset(lines[0], vec2(coordinate.x, 2), dFdx(coordinate.x), dFdy(coordinate.x), 1);
    color.xy += textureQueryLod(lines[0], coordinate.x);
    color.x += texture(shadowLine, vec3(coordinate.x, 0, 0.4));
    color.y += textureProj(shadowLine, vec4(coordinate.x, 0, 0.4, 2));
    color.z += texture(shadowArray, vec3(coordinate, 0.4));
    color.w += textureGradOffset(shadowArray, vec3(coordinate, 0.4), 0.1, 0.2, 1);
}
