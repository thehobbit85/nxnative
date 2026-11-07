// Guest Vulkan -> Android Vulkan. ARM64 data layouts and handles are native;
// procedure lookup exposes only our guarded wrappers, never arbitrary driver code.
// Allocation callbacks follow the Apple backend policy (host allocation). Guest
// callbacks in pNext are not handed to native driver threads without a guest gate.
#include "guest_vk.h"

static const char *const instance_extensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
// Device extensions are added as complete command families are implemented.
static const char *const device_extensions[] = {
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
};
static const unsigned device_extension_count = sizeof device_extensions / sizeof *device_extensions;

static int listed(const char *name, const char *const *names, unsigned count) {
    for (unsigned i = 0; i < count; ++i) if (!strcmp(name, names[i])) return 1;
    return 0;
}
static VkResult filtered_extensions(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *out) {
    if (layer) { *count = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    uint32_t n = 0;
    VkResult r = pd ? vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) : vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *native = calloc(n ? n : 1, sizeof *native);
    if (!native) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = pd ? vkEnumerateDeviceExtensionProperties(pd, NULL, &n, native) : vkEnumerateInstanceExtensionProperties(NULL, &n, native);
    if (r != VK_SUCCESS) { free(native); return r; }
    const char *const *names = pd ? device_extensions : instance_extensions;
    unsigned allowed = pd ? device_extension_count : sizeof instance_extensions / sizeof *instance_extensions;
    uint32_t written = 0, total = 0, capacity = out ? *count : 0;
    for (uint32_t i = 0; i < n; ++i) if (listed(native[i].extensionName, names, allowed)) {
        if (out && written < capacity) out[written++] = native[i];
        ++total;
    }
    free(native); *count = out ? written : total;
    return out && written < total ? VK_INCOMPLETE : VK_SUCCESS;
}
static int extensions_allowed(uint32_t count, const char *const *requested, int device) {
    for (uint32_t i = 0; i < count; ++i)
        if (!listed(requested[i], device ? device_extensions : instance_extensions,
                    device ? device_extension_count : sizeof instance_extensions / sizeof *instance_extensions)) return 0;
    return 1;
}

GVK(vkEnumerateInstanceVersion, GVK_GLOBAL, VkResult, uint32_t *version) {
    VkResult r = vkEnumerateInstanceVersion(version);
    if (r == VK_SUCCESS && *version > VK_API_VERSION_1_1) *version = VK_API_VERSION_1_1;
    return r;
}
GVK(vkEnumerateInstanceLayerProperties, GVK_GLOBAL, VkResult, uint32_t *count, VkLayerProperties *out) {
    (void)out; *count = 0; return VK_SUCCESS;
}
GVK(vkEnumerateInstanceExtensionProperties, GVK_GLOBAL, VkResult, const char *layer, uint32_t *count, VkExtensionProperties *out) {
    return filtered_extensions(VK_NULL_HANDLE, layer, count, out);
}
GVK(vkCreateInstance, GVK_GLOBAL, VkResult, const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *allocation, VkInstance *out) {
    (void)allocation; *out = VK_NULL_HANDLE;
    if (ci->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!extensions_allowed(ci->enabledExtensionCount, ci->ppEnabledExtensionNames, 0)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (ci->pApplicationInfo && (ci->pApplicationInfo->apiVersion >> 12) > (VK_API_VERSION_1_1 >> 12)) return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (ci->pNext) { LOG("[RED] guest Vulkan instance pNext is not bridged yet"); return VK_ERROR_INITIALIZATION_FAILED; }
    return vkCreateInstance(ci, NULL, out);
}
GVK(vkDestroyInstance, GVK_INSTANCE, void, VkInstance instance, const VkAllocationCallbacks *allocation) {
    (void)allocation; vkDestroyInstance(instance, NULL);
}
GVK(vkEnumeratePhysicalDevices, GVK_INSTANCE, VkResult, VkInstance instance, uint32_t *count, VkPhysicalDevice *out) {
    return vkEnumeratePhysicalDevices(instance, count, out);
}
GVK(vkGetPhysicalDeviceProperties, GVK_INSTANCE, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties *out) {
    vkGetPhysicalDeviceProperties(pd, out);
    if (out->apiVersion > VK_API_VERSION_1_1) out->apiVersion = VK_API_VERSION_1_1;
}
GVK(vkGetPhysicalDeviceProperties2, GVK_INSTANCE, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *out) {
    vkGetPhysicalDeviceProperties2(pd, out);
    if (out->properties.apiVersion > VK_API_VERSION_1_1) out->properties.apiVersion = VK_API_VERSION_1_1;
}
GVK_ALIAS(vkGetPhysicalDeviceProperties2KHR, vkGetPhysicalDeviceProperties2, GVK_INSTANCE);
GVK(vkGetPhysicalDeviceFeatures, GVK_INSTANCE, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures *out) { vkGetPhysicalDeviceFeatures(pd, out); }
GVK(vkGetPhysicalDeviceFeatures2, GVK_INSTANCE, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *out) { vkGetPhysicalDeviceFeatures2(pd, out); }
GVK_ALIAS(vkGetPhysicalDeviceFeatures2KHR, vkGetPhysicalDeviceFeatures2, GVK_INSTANCE);
GVK(vkGetPhysicalDeviceMemoryProperties, GVK_INSTANCE, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *out) { vkGetPhysicalDeviceMemoryProperties(pd, out); }
GVK(vkGetPhysicalDeviceMemoryProperties2, GVK_INSTANCE, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *out) { vkGetPhysicalDeviceMemoryProperties2(pd, out); }
GVK_ALIAS(vkGetPhysicalDeviceMemoryProperties2KHR, vkGetPhysicalDeviceMemoryProperties2, GVK_INSTANCE);
GVK(vkGetPhysicalDeviceQueueFamilyProperties, GVK_INSTANCE, void, VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties *out) {
    vkGetPhysicalDeviceQueueFamilyProperties(pd, count, out);
}
GVK(vkGetPhysicalDeviceQueueFamilyProperties2, GVK_INSTANCE, void, VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties2 *out) {
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, count, out);
}
GVK_ALIAS(vkGetPhysicalDeviceQueueFamilyProperties2KHR, vkGetPhysicalDeviceQueueFamilyProperties2, GVK_INSTANCE);
GVK(vkGetPhysicalDeviceFormatProperties, GVK_INSTANCE, void, VkPhysicalDevice pd, VkFormat format, VkFormatProperties *out) { vkGetPhysicalDeviceFormatProperties(pd, format, out); }
GVK(vkGetPhysicalDeviceFormatProperties2, GVK_INSTANCE, void, VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *out) { vkGetPhysicalDeviceFormatProperties2(pd, format, out); }
GVK_ALIAS(vkGetPhysicalDeviceFormatProperties2KHR, vkGetPhysicalDeviceFormatProperties2, GVK_INSTANCE);
GVK(vkGetPhysicalDeviceImageFormatProperties, GVK_INSTANCE, VkResult, VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *out) {
    return vkGetPhysicalDeviceImageFormatProperties(pd, format, type, tiling, usage, flags, out);
}
GVK(vkGetPhysicalDeviceImageFormatProperties2, GVK_INSTANCE, VkResult, VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out) {
    return vkGetPhysicalDeviceImageFormatProperties2(pd, info, out);
}
GVK_ALIAS(vkGetPhysicalDeviceImageFormatProperties2KHR, vkGetPhysicalDeviceImageFormatProperties2, GVK_INSTANCE);
GVK(vkGetPhysicalDeviceSparseImageFormatProperties, GVK_INSTANCE, void, VkPhysicalDevice pd, VkFormat format, VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *count, VkSparseImageFormatProperties *out) {
    vkGetPhysicalDeviceSparseImageFormatProperties(pd, format, type, samples, usage, tiling, count, out);
}
GVK(vkGetPhysicalDeviceSparseImageFormatProperties2, GVK_INSTANCE, void, VkPhysicalDevice pd, const VkPhysicalDeviceSparseImageFormatInfo2 *info, uint32_t *count, VkSparseImageFormatProperties2 *out) {
    vkGetPhysicalDeviceSparseImageFormatProperties2(pd, info, count, out);
}
GVK_ALIAS(vkGetPhysicalDeviceSparseImageFormatProperties2KHR, vkGetPhysicalDeviceSparseImageFormatProperties2, GVK_INSTANCE);

GVK(vkEnumerateDeviceLayerProperties, GVK_INSTANCE, VkResult, VkPhysicalDevice pd, uint32_t *count, VkLayerProperties *out) {
    (void)pd; (void)out; *count = 0; return VK_SUCCESS;
}
GVK(vkEnumerateDeviceExtensionProperties, GVK_INSTANCE, VkResult, VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *out) {
    return filtered_extensions(pd, layer, count, out);
}
GVK(vkCreateDevice, GVK_INSTANCE, VkResult, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *allocation, VkDevice *out) {
    (void)allocation; *out = VK_NULL_HANDLE;
    if (ci->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!extensions_allowed(ci->enabledExtensionCount, ci->ppEnabledExtensionNames, 1)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    // Only core data-only device creation chains are accepted at this stage.
    // EXT_device_memory_report, for example, contains a guest callback.
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
        if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            LOG("[RED] guest Vulkan device pNext type %u is not bridged yet", p->sType);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    VkResult r = vkCreateDevice(pd, ci, NULL, out);
    if (r != VK_SUCCESS) return r;
    r = guest_vk_track_device(pd, *out);
    if (r != VK_SUCCESS) { vkDestroyDevice(*out, NULL); *out = VK_NULL_HANDLE; }
    return r;
}
GVK(vkDestroyDevice, GVK_DEVICE, void, VkDevice device, const VkAllocationCallbacks *allocation) { (void)allocation; guest_vk_destroy_device(device); }
GVK(vkGetDeviceQueue, GVK_DEVICE, void, VkDevice device, uint32_t family, uint32_t index, VkQueue *out) { vkGetDeviceQueue(device, family, index, out); }

static const guest_vk_proc *find_proc(const char *name) {
    if (!name || strncmp(name, "vk", 2)) return NULL;
    unsigned long bytes; const guest_vk_proc *entries = DM_SECTION_DATA(dmguestvk, &bytes);
    for (unsigned long i = 0; i < bytes / sizeof *entries; ++i) if (!strcmp(name, entries[i].name)) return &entries[i];
    return NULL;
}
GVK(vkGetInstanceProcAddr, GVK_GLOBAL, PFN_vkVoidFunction, VkInstance instance, const char *name) {
    const guest_vk_proc *p = find_proc(name);
    if (!p || (!instance && p->scope != GVK_GLOBAL)) return NULL;
    // Core/global wrappers are exported statically, but extension availability and
    // enablement remain the native loader's per-instance decisions.
    return vkGetInstanceProcAddr(instance, name) ? p->fn : NULL;
}
GVK(vkGetDeviceProcAddr, GVK_DEVICE, PFN_vkVoidFunction, VkDevice device, const char *name) {
    const guest_vk_proc *p = find_proc(name);
    if (!device || !p || p->scope != GVK_DEVICE) return NULL;
    return vkGetDeviceProcAddr(device, name) ? p->fn : NULL;
}
