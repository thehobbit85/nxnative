// The shared guest C2 ABI, following the Apple replay's tex_slot_h/c2_handle.
#ifndef DM_VK_DRIVER_C2_H
#define DM_VK_DRIVER_C2_H
#include "../dmsh_abi.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint64_t dm_vk_texture_slot(const uint64_t *textures, const uint64_t *images, unsigned slot) {
    unsigned image0 = (DMSH_C2_IMG - DMSH_C2_TEX) / DMSH_C2_ROW;
    if (slot >= image0 && slot - image0 < DMSH_C2_IMG_SLOTS && images[slot-image0]) return images[slot-image0];
    return slot < DMSH_SEPTEX_SLOT0 + DMSH_C2_SEP_SLOTS ? textures[slot] : 0;
}
static inline uint64_t dm_vk_c2_handle(const uint64_t *textures, const uint64_t *images, const uint64_t *samplers, uint64_t off) {
    if (off >= DMSH_C2_TEX && off < DMSH_C2_SEPTEX && !((off-DMSH_C2_TEX) % DMSH_C2_ROW))
        return dm_vk_texture_slot(textures, images, (unsigned)((off-DMSH_C2_TEX) / DMSH_C2_ROW));
    if (off >= DMSH_C2_SEPTEX && off < DMSH_C2_SAMP && !((off-DMSH_C2_SEPTEX) % DMSH_C2_ROW))
        return textures[DMSH_SEPTEX_SLOT0 + (off-DMSH_C2_SEPTEX) / DMSH_C2_ROW];
    if (off >= DMSH_C2_SAMP && off < DMSH_C2_SAMP + DMSH_C2_ROW*DMSH_C2_SEP_SLOTS && !((off-DMSH_C2_SAMP) % DMSH_C2_ROW))
        return samplers[(off-DMSH_C2_SAMP) / DMSH_C2_ROW];
    return 0;
}
static inline void dm_vk_c2_fill(void *out, size_t size, const uint64_t *textures, const uint64_t *images, const uint64_t *samplers) {
    memset(out, 0, size);
    for (size_t off = DMSH_C2_TEX; off < DMSH_C2_SAMP + DMSH_C2_ROW*DMSH_C2_SEP_SLOTS && off <= size && size-off >= 4; off += DMSH_C2_ROW) {
        uint32_t word = (uint32_t)dm_vk_c2_handle(textures, images, samplers, off);
        memcpy((uint8_t *)out+off, &word, sizeof word);
    }
}
#endif
