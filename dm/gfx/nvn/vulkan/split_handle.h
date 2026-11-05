// Additional scalar handle words; distinct from the paired sampler's source.
#ifndef DM_VK_SPLIT_HANDLE_H
#define DM_VK_SPLIT_HANDLE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int reg, off; } dm_vk_uniform_word;
typedef struct { dm_vk_uniform_word texture, sampler; } dm_vk_split_handle;
static inline int dm_vk_split_word(const char *line, const char *token, dm_vk_uniform_word *word) {
    *word = (dm_vk_uniform_word){-1, -1};
    const char *start = strstr(line, token);
    if (!start) return 1;
    start += strlen(token);
    if (strstr(start, token) || *start < '0' || *start > '9') return 0;
    char *end;
    unsigned long reg = strtoul(start, &end, 10);
    if (reg >= 16 || *end != ':' || end[1] < '0' || end[1] > '9') return 0;
    unsigned long off = strtoul(end + 1, &end, 10);
    if (off > INT32_MAX || (*end && *end != ' ')) return 0;
    *word = (dm_vk_uniform_word){(int)reg, (int)off};
    return 1;
}
static inline int dm_vk_split_handle_parse(const char *line, dm_vk_split_handle *out) {
    return dm_vk_split_word(line, " twin_tex=", &out->texture) && dm_vk_split_word(line, " twin_samp=", &out->sampler);
}
#endif
