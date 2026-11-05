#ifndef DM_VK_GEOMETRY_CONTRACT_H
#define DM_VK_GEOMETRY_CONTRACT_H
// Geometry is logical NVN stage 2; these products execute as compute or vertex.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct {
    int reader, capture;
    uint32_t topology, stride, vertices, invocations, output, index_stride;
} dm_vk_geometry;
static const char *dm_vk_geometry_line(const char *line, size_t n, int model, dm_vk_geometry *geometry) {
    size_t at;
    geometry->capture = 0;
    if (n >= 17 && !memcmp(line, "geometry compute ", 17)) {
        if (model != 5) return "geometry compute needs compute stage";
        geometry->reader = 0; at = 17;
    } else if (n >= 16 && !memcmp(line, "geometry reader ", 16)) {
        if (model != 0) return "geometry reader needs vertex stage";
        geometry->reader = 1; at = 16;
    } else if (n >= 17 && !memcmp(line, "geometry capture ", 17)) {
        if (model != 0) return "geometry capture needs vertex stage";
        geometry->reader = 1; geometry->capture = 1; at = 17;
    } else return "invalid geometry helper role";
    uint32_t values[6]; unsigned count = 0;
    while (at < n) {
        if (count == 6 || line[at] < '0' || line[at] > '9') return "invalid geometry field";
        uint32_t value = 0;
        do {
            if (value > (INT32_MAX - (uint32_t)(line[at] - '0')) / 10) return "invalid geometry field";
            value = value * 10 + (uint32_t)(line[at++] - '0');
        } while (at < n && line[at] >= '0' && line[at] <= '9');
        values[count++] = value;
        if (at < n && (line[at++] != ' ' || at == n)) return "invalid geometry field";
    }
    if (count != 6 || values[0] >= 5 || values[1] < 5 || values[1] > 144 || !values[2] || values[2] > 4095 ||
        !values[3] || values[3] > 255 || (values[4] != 1 && values[4] != 6 && values[4] != 7) || values[5] != values[2] * 2)
        return "invalid geometry output layout";
    geometry->topology = values[0]; geometry->stride = values[1]; geometry->vertices = values[2];
    geometry->invocations = values[3]; geometry->output = values[4]; geometry->index_stride = values[5];
    return NULL;
}
#endif
