// Context for shaders that index logical slots or the bound pools themselves.
#ifndef DM_VK_DRIVER_C2_MODES_H
#define DM_VK_DRIVER_C2_MODES_H
#include "driver_c2.h"
enum { DM_VK_C2_HANDLES, DM_VK_C2_SLOTS, DM_VK_C2_POOLS };

// TEXD uses combined texture slot k, then separate texture k. SAMPD prefers
// separate sampler k, then the sampler carried by combined texture slot k.
static inline uint64_t dm_vk_slot_texture(const uint64_t *textures, unsigned slot) {
    return slot < 32 ? (textures[slot] ? textures[slot] : textures[DMSH_SEPTEX_SLOT0 + slot]) : 0;
}
static inline uint64_t dm_vk_slot_sampler(const uint64_t *textures, const uint64_t *samplers, unsigned slot) {
    if (slot >= 32) return 0;
    uint64_t sampler = samplers[slot] >> 20;
    return sampler ? sampler : textures[slot] >> 20;
}
static inline uint32_t dm_vk_pool_local(uint64_t handle, unsigned texture_base, unsigned sampler_base) {
    uint64_t texture = handle & 0xfffff, sampler = (handle >> 20) & 0xfff;
    return (uint32_t)((texture ? texture - texture_base : 0) | ((sampler ? sampler - sampler_base : 0) << 20));
}
static inline int dm_vk_c2_fill_mode(void *out, size_t size, const uint64_t *textures, const uint64_t *images,
                                    const uint64_t *samplers, unsigned mode, unsigned texture_base, unsigned sampler_base) {
    if (mode > DM_VK_C2_POOLS || (size && !out) || !textures || !images || !samplers) return 0;
    if (!size) return 1;
    memset(out, 0, size);
    for (size_t off = DMSH_C2_TEX; off < DMSH_C2_SAMP + DMSH_C2_ROW*DMSH_C2_SEP_SLOTS && off <= size && size-off >= 4; off += DMSH_C2_ROW) {
        uint64_t handle = dm_vk_c2_handle(textures, images, samplers, off);
        uint32_t word = (uint32_t)handle;
        int image = off >= DMSH_C2_IMG && off < DMSH_C2_IMG + DMSH_C2_ROW*DMSH_C2_IMG_SLOTS &&
                    images[(off-DMSH_C2_IMG)/DMSH_C2_ROW];
        // The image table retains its recorded handles, even when its words
        // overlap high combined texture slots. Only texture/sampler rows remap.
        if (!image && mode == DM_VK_C2_POOLS) word = dm_vk_pool_local(handle, texture_base, sampler_base);
        else if (!image && mode == DM_VK_C2_SLOTS) {
            unsigned slot;
            if (off < DMSH_C2_SEPTEX) {
                slot = (unsigned)((off-DMSH_C2_TEX)/DMSH_C2_ROW);
                word = handle ? slot | (handle >> 20 ? slot << 20 : 0) : 0;
            } else if (off < DMSH_C2_SAMP) {
                slot = (unsigned)((off-DMSH_C2_SEPTEX)/DMSH_C2_ROW); word = handle ? slot : 0;
            } else {
                slot = (unsigned)((off-DMSH_C2_SAMP)/DMSH_C2_ROW); word = handle ? slot << 20 : 0;
            }
        }
        memcpy((uint8_t *)out+off, &word, sizeof word);
    }
    return 1;
}
#endif
