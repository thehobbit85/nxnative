// The VI layer determines the output shape even when the game crops its texture.
#pragma once
#include <math.h>
#include <stdint.h>

typedef struct { int32_t x, y, w, h; } vk_present_rect;

static inline uint64_t vk_present_shape(uint32_t w, uint32_t h) {
    return w && h ? w | ((uint64_t)h << 32) : 0;
}

static inline vk_present_rect vk_present_fit(uint64_t screen, uint64_t layer, uint32_t tw, uint32_t th,
                                            int32_t dw, int32_t dh, int stretch) {
    double aspect = layer ? (double)(uint32_t)layer / (layer >> 32) : 16.0 / 9;
    if (screen && th) {
        double requested = (double)(uint32_t)screen / (screen >> 32);
        double texture = (double)tw / th;
        if (fabs(texture / requested - 1) < 0.01) aspect = texture;
    }
    if (stretch || fabs(((double)dw / dh) / aspect - 1) < 0.01)
        return (vk_present_rect){0, 0, dw, dh};
    double w = fmin(dw, dh * aspect), h = fmin(dh, dw / aspect);
    return (vk_present_rect){floor((dw - w) / 2), floor((dh - h) / 2), round(w), round(h)};
}
