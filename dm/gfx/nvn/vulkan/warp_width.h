// Subgroup width requirements computed by the Android shader exporter.
#ifndef DM_VK_WARP_WIDTH_H
#define DM_VK_WARP_WIDTH_H
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t min, max; } dm_vk_warp_width;

static inline int dm_vk_warp_line(const char *line, size_t n, dm_vk_warp_width *width) {
    static const struct { const char *row; dm_vk_warp_width width; } rows[] = {
        {"warp independent",{0,0}}, {"warp 4 128",{4,128}}, {"warp 8 128",{8,128}}, {"warp 16 128",{16,128}},
        {"warp 32 128",{32,128}}, {"warp 32 32",{32,32}}
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        if (n == strlen(rows[i].row) && !memcmp(line, rows[i].row, n)) {
            *width = rows[i].width;
            return 1;
        }
    }
    return 0;
}

// The contract reader has already checked every instruction length.
static inline const uint32_t *dm_vk_warp_constant(const uint32_t *words, size_t count, uint32_t id) {
    for (size_t at = 5; at < count; at += words[at] >> 16) {
        uint32_t op = words[at] & 0xffff;
        if (op == 54) break;
        if ((op == 43 || op == 44 || op == 50 || op == 51) && (words[at] >> 16) >= 3 && words[at + 2] == id)
            return words + at;
    }
    return NULL;
}

// WorkgroupSize takes precedence over LocalSize. The renderer supplies no
// specialization map. Unknown sizes can use the default but cannot request
// a narrower subgroup that might exceed maxComputeWorkgroupSubgroups.
static inline uint32_t dm_vk_warp_local(const uint32_t *words, size_t count, uint32_t builtin, const uint32_t local[3]) {
    const uint32_t *group = builtin ? dm_vk_warp_constant(words, count, builtin) : NULL;
    if (builtin && (!group || (group[0] >> 16) != 6 || ((group[0] & 0xffff) != 44 && (group[0] & 0xffff) != 51)))
        return UINT32_MAX;
    uint32_t product = 1;
    for (unsigned k = 0; k < 3; k++) {
        uint32_t n = local[k];
        if (group) {
            const uint32_t *component = dm_vk_warp_constant(words, count, group[3 + k]);
            if (!component || (component[0] >> 16) != 4 || ((component[0] & 0xffff) != 43 && (component[0] & 0xffff) != 50))
                return UINT32_MAX;
            n = component[3];
        }
        if (!n || n > UINT32_MAX / product) return UINT32_MAX;
        product *= n;
    }
    return product;
}

// Return 0 for the device default, a positive size for pipeline creation,
// or -1 when the fixed/default and selectable sizes cannot meet the contract.
static inline int dm_vk_warp_select(dm_vk_warp_width width, uint32_t native, uint32_t first, uint32_t last,
                                    uint32_t local_count, uint32_t max_groups) {
    if (!width.min || (native >= width.min && native <= width.max)) return 0;
    for (uint32_t size = width.min; size <= width.max && size <= 128; size *= 2) {
        if (size >= first && size <= last && (local_count != UINT32_MAX && (uint64_t)size * max_groups >= local_count))
            return (int)size;
    }
    return -1;
}
#endif
