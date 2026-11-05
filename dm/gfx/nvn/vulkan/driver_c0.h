// Guest C0 bytes shared with the Apple ABI; Vulkan supplies the current state.
#ifndef DM_VK_DRIVER_C0_H
#define DM_VK_DRIVER_C0_H
#include "../nvn_obj.h"
#include <stddef.h>

static inline int dm_vk_c0_fill(void *out, size_t size, int stage,
                              const p_bind_buf *storage, const p_bind_buf *uniform,
                              const uint32_t dispatch[3], uint32_t base_instance,
                              const uint8_t *sample_positions, unsigned samples, int nvn2) {
    static const unsigned bases[] = {DMSH_C0_SSBO_VS, DMSH_C0_SSBO_PS, DMSH_C0_SSBO_GS,
                                    DMSH_C0_SSBO_TS, DMSH_C0_SSBO_MS, DMSH_C0_SSBO_CS};
    if (size < DMSH_DRV_SIZE || stage < 0 || stage >= 6 ||
        (stage == 1 && (!sample_positions || !samples || samples > DMSH_C0_SAMPLE_BYTES || (samples & (samples-1))))) return 0;
    uint8_t *bytes = out;
    memset(bytes, 0, size);
    for (unsigned r = 0; r < DMSH_C0_SSBO_SLOTS; r++) {
        uint32_t length = (uint32_t)storage[r].size;
        memcpy(bytes + bases[stage] + DMSH_C0_ROW*r, &storage[r].addr, 8);
        memcpy(bytes + bases[stage] + DMSH_C0_ROW*r + 8, &length, 4);
    }
    if (stage == 5) {
        memcpy(bytes + DMSH_C0_DISPATCH, dispatch, 12);
        if (nvn2) memcpy(bytes + DMSH_C0_DISPATCH_NVN2, dispatch, 12);
        for (unsigned r = 0; r < DMSH_C0_CS_UBO_SLOTS; r++) {
            uint32_t length = (uint32_t)uniform[r].size;
            memcpy(bytes + DMSH_C0_CS_UBO + DMSH_C0_ROW*r, &uniform[r].addr, 8);
            memcpy(bytes + DMSH_C0_CS_UBO + DMSH_C0_ROW*r + 8, &length, 4);
        }
    } else {
        const float maximum_point_size = 2047.0f;
        memcpy(bytes + DMSH_C0_POINT_SIZE, &maximum_point_size, 4);
        memcpy(bytes + DMSH_C0_BASE_INSTANCE, &base_instance, 4);
        if (stage == 1) {
            uint32_t log2_samples = 0;
            for (unsigned n = samples; n > 1; n >>= 1) log2_samples++;
            const uint32_t info[] = {samples-1, log2_samples, 0, 0};
            memcpy(bytes + DMSH_C0_SAMPLE_TABLE, sample_positions, samples);
            memcpy(bytes + DMSH_C0_SAMPLE_INFO, info, sizeof info);
        }
    }
    return 1;
}
#endif
