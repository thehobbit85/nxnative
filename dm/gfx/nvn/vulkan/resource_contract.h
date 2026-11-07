#ifndef DM_VK_RESOURCE_CONTRACT_H
#define DM_VK_RESOURCE_CONTRACT_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Frozen Apple reflection admits 128 resources. Descriptor array length and
// physical-device limits are separate from this offline interface capacity.
#define DM_VK_MAX_BINDINGS 128
typedef struct { unsigned char fetch1d, fetch1d_size; } dm_vk_resource_shape;

static inline const char *dm_vk_resource_line(const char *line, size_t n, dm_vk_resource_shape *shape) {
    char copy[1024], type[8], kind[16], sampler[16]; int binding, count, reg, off, sreg, tail = 0;
    memset(shape, 0, sizeof *shape);
    if (n >= sizeof copy) return "oversized resource record";
    memcpy(copy, line, n); copy[n] = 0;
    if (sscanf(copy, "b %d %7s %d %15s %d %d %15s %d%n", &binding, type, &count, kind, &reg, &off, sampler, &sreg, &tail) != 8 ||
        binding < 0 || count < 1) return "invalid resource record";
    int view = -1;
    for (size_t at = (size_t)tail; at < n;) {
        if (copy[at++] != ' ') return "invalid resource separator";
        size_t start = at; while (at < n && copy[at] != ' ') at++;
        size_t length = at - start; const char *field = copy + start;
        if (length >= 8 && !memcmp(field, "fetch1d=", 8)) {
            if (length != 9 || field[8] != '1' || shape->fetch1d++) return "invalid wrapped fetch flag";
        } else if (length >= 13 && !memcmp(field, "fetch1d_size=", 13)) {
            if (length != 14 || field[13] != '1' || shape->fetch1d_size++) return "invalid wrapped fetch size flag";
        } else if (length == 6 && !memcmp(field, "view=", 5) && field[5] >= '0' && field[5] <= '6') view = field[5] - '0';
    }
    int sampled=!strcmp(kind,"tex") || !strcmp(kind,"tex_ubo") || !strcmp(kind,"tex_slots") ||
        !strcmp(kind,"tex_c2a") || !strcmp(kind,"tex_htab") || !strcmp(kind,"tex_pool");
    if ((shape->fetch1d || shape->fetch1d_size) &&
        (!sampled || (strcmp(type, "tex") && strcmp(type, "cis")) || view != 1 || !shape->fetch1d))
        return "invalid wrapped fetch binding";
    if (!strcmp(kind, "tex_slots") || !strcmp(kind, "samp_slots")) {
        int sampler_array = !strcmp(kind, "samp_slots");
        if (count > 32 || reg != -1 || (sampler_array ? strcmp(type, "samp") :
            strcmp(type, "tex") && strcmp(type, "cis") && strcmp(type, "tbuf") && strcmp(type, "utb")))
            return "invalid logical slot array binding";
    }
    return NULL;
}
#endif
