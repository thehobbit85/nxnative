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
// TEXD/SAMPD arrays are indexed by logical slots. Keep overlapping image words
// as real image handles, following the reference's per-shader C2 convention.
static inline void dm_vk_c2_logical(void *out,size_t size,const uint64_t *textures,const uint64_t *images,const uint64_t *samplers) {
    for (unsigned slot=0;slot<DMSH_C2_TEX_SLOTS;slot++) {
        uint64_t handle=textures[slot];
        if (slot>=32 && (!handle || (slot-32<DMSH_C2_IMG_SLOTS && images[slot-32]))) continue;
        size_t offset=DMSH_C2_TEX+DMSH_C2_ROW*slot;
        uint32_t word=handle ? slot | (handle>>20 ? slot<<20 : 0) : 0;
        if (offset<=size && size-offset>=4) memcpy((uint8_t *)out+offset,&word,4);
    }
    for (unsigned slot=0;slot<DMSH_C2_SEP_SLOTS;slot++) {
        size_t texture_offset=DMSH_C2_SEPTEX+DMSH_C2_ROW*slot;
        size_t sampler_offset=DMSH_C2_SAMP+DMSH_C2_ROW*slot;
        uint32_t texture=textures[DMSH_SEPTEX_SLOT0+slot] ? slot : 0;
        uint32_t sampler=samplers[slot] ? slot<<20 : 0;
        if (texture_offset<=size && size-texture_offset>=4) memcpy((uint8_t *)out+texture_offset,&texture,4);
        if (sampler_offset<=size && size-sampler_offset>=4) memcpy((uint8_t *)out+sampler_offset,&sampler,4);
    }
}

#endif
