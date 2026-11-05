#ifndef DM_VK_FEEDBACK_GATHER_H
#define DM_VK_FEEDBACK_GATHER_H
// One feedback output per dispatch: three storage buffers and one uniform
// block fit Vulkan 1.1's minimum per-stage descriptor limits. Bind capture,
// ordered indices, destination and this block at set 0 bindings 0,1,2,3.
#include "shader_contract.h"

typedef struct {
    uint32_t source_offset, source_records, order_offset, count;
    uint32_t destination_offset, stride, ordered, first;
    uint32_t words[512];
} dm_vk_feedback_gather;
_Static_assert(offsetof(dm_vk_feedback_gather, words) == 32, "feedback std140 map offset");
_Static_assert(sizeof(dm_vk_feedback_gather) == 2080, "feedback std140 uniform size");

// Offsets and stride are words; first is an index in the capture record array.
// Ordered indices are relative to first. Callers set counts/capacities after
// common whole-primitive truncation; this helper neither advances nor owns
// NVN's feedback pause/resume state. A zero/invalid layout has no dispatch.
static int dm_vk_feedback_gather_layout(const dm_vk_feedback *layout, unsigned buffer, dm_vk_feedback_gather *control) {
    if (buffer >= 4 || !layout->count[buffer] || layout->count[buffer] > 128) return 0;
    unsigned words = 0;
    for (unsigned i = 0; i < layout->count[buffer]; i++) {
        unsigned location = layout->location[buffer][i], count = layout->components[buffer][i];
        if (location >= 32 || !count || count > 4) return 0;
        words += count;
    }
    if (layout->stride[buffer] != words * 4) return 0;
    control->stride = words;
    unsigned word = 0;
    for (unsigned i = 0; i < layout->count[buffer]; i++)
        for (unsigned k = 0; k < layout->components[buffer][i]; k++)
            control->words[word++] = 16u + layout->location[buffer][i] * 4u + k;
    return 1;
}
#endif
