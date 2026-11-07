// Android's lifecycle drains every guest Vulkan device, independently of the
// NVN executor's native device. Registration and destruction serialize with the
// drain; normal command submission does not hold this registry lock.
#include "guest_vk.h"

typedef struct guest_device {
    VkDevice device;
    VkPhysicalDevice physical;
    struct guest_device *next;
} guest_device;
static guest_device *devices;
static pthread_mutex_t devices_mutex = PTHREAD_MUTEX_INITIALIZER;

VkResult guest_vk_track_device(VkPhysicalDevice physical, VkDevice device) {
    guest_device *record = malloc(sizeof *record);
    if (!record) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *record = (guest_device){.device = device, .physical = physical};
    dm_host_mutex_lock(&devices_mutex);
    record->next = devices; devices = record;
    pthread_mutex_unlock(&devices_mutex);
    return VK_SUCCESS;
}
void guest_vk_destroy_device(VkDevice device) {
    dm_host_mutex_lock(&devices_mutex);
    guest_device **link = &devices;
    while (*link && (*link)->device != device) link = &(*link)->next;
    guest_device *record = *link;
    if (record) *link = record->next;
    vkDestroyDevice(device, NULL);
    free(record);
    pthread_mutex_unlock(&devices_mutex);
}
void dm_vk_drain(void) {
    // dm_gfx_suspend has closed the graphics gate and waited for submit/present
    // calls to leave it. No queue operation can start while these devices drain.
    dm_host_mutex_lock(&devices_mutex);
    for (guest_device *p = devices; p; p = p->next) {
        VkResult r = vkDeviceWaitIdle(p->device);
        if (r != VK_SUCCESS) LOG("[RED] guest Vulkan device drain failed: %d", r);
    }
    pthread_mutex_unlock(&devices_mutex);
}
