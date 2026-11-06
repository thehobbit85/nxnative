#version 450
layout(set=0, binding=0) uniform sampler2D sourceImage;
layout(push_constant) uniform DrawTexture {
    vec2 dstOrigin;
    vec2 srcOrigin;
    vec2 texelsPerPixel;
    int reduction, linear, level0, level1;
} region;
layout(location=0) out vec4 color;

vec4 combine(vec4 a, vec4 b) {
    return region.reduction == 1 ? min(a,b) : max(a,b);
}

vec4 reduceLevel(vec2 uv, int level) {
    if (region.linear == 0) return textureLod(sourceImage, uv, float(level));
    vec2 size = vec2(textureSize(sourceImage, level));
    vec2 position = uv * size - 0.5;
    vec2 center = floor(position) + 0.5;
    vec2 fraction = fract(position);
    vec4 result = textureLod(sourceImage, center / size, float(level));
    if (fraction.x > 0) result = combine(result, textureLod(sourceImage, (center + vec2(1,0)) / size, float(level)));
    if (fraction.y > 0) result = combine(result, textureLod(sourceImage, (center + vec2(0,1)) / size, float(level)));
    if (fraction.x > 0 && fraction.y > 0)
        result = combine(result, textureLod(sourceImage, (center + 1.0) / size, float(level)));
    return result;
}

void main() {
    vec2 texel = region.srcOrigin + (gl_FragCoord.xy - region.dstOrigin) * region.texelsPerPixel;
    vec2 uv = texel / vec2(textureSize(sourceImage, 0));
    if (region.reduction == 0) {
        color = texture(sourceImage, uv);
    } else {
        color = reduceLevel(uv, region.level0);
        if (region.level1 != region.level0) color = combine(color, reduceLevel(uv, region.level1));
    }
}
