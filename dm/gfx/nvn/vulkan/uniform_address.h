// A guest storage address occupies eight bytes, even on a four-byte boundary.
#ifndef DM_VK_UNIFORM_ADDRESS_H
#define DM_VK_UNIFORM_ADDRESS_H
#include <stdint.h>

// Return the address of the field, not its contents. The caller makes all
// eight bytes CPU-visible and checks pool membership before reading them.
static inline uint64_t dm_vk_uniform_address_field(uint64_t base, uint64_t size, int64_t offset) {
    if (!base || offset < 0 || size < 8 || (uint64_t)offset > size - 8) return 0;
    if (base > UINT64_MAX - 8 || (uint64_t)offset > UINT64_MAX - 8 - base) return 0;
    return base + (uint64_t)offset;
}
#endif
