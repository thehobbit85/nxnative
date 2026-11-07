// The container and relocated records are shared with Metal captures. Vulkan
// pixel-format/image-type enums and reflection hashes have their own contract.
#pragma once
#include "../nvn_capture.h"
#define CAP_VK_IFACE 3
#define CAP_VK_RESULT CAP_TAG('V', 'K', 'O', 'K') // uint32_t, 1 only for a complete capture
