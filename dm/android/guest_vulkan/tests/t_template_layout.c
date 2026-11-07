// Check arbitrary guest template byte layouts independently of a driver's
// template implementation. Capture the ordinary Vulkan writes at the boundary.
#define vkUpdateDescriptorSets capture_writes
#include "../descriptor_templates.c"
#undef vkUpdateDescriptorSets
#include <assert.h>
int dm_guest_as_contains(uintptr_t p) { (void)p; return 0; }
static unsigned captured;
void capture_writes(VkDevice device, uint32_t count, const VkWriteDescriptorSet *w, uint32_t copies, const VkCopyDescriptorSet *copy) {
    (void)device; (void)copy; assert(count == 1 && !copies && w->dstSet == (VkDescriptorSet)9);
    assert(w->dstBinding == captured * 3 && w->dstArrayElement == 1);
    if (captured == 0) {
        assert(w->descriptorCount == 9 && w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        for (unsigned i = 0; i < 9; ++i) assert(w->pBufferInfo[i].buffer == (VkBuffer)17 && w->pBufferInfo[i].offset == 32 && w->pBufferInfo[i].range == 96);
    } else if (captured == 1) {
        assert(w->descriptorCount == 2 && w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        for (unsigned i = 0; i < 2; ++i) assert(w->pImageInfo[i].sampler == (VkSampler)(uintptr_t)(20 + i) && w->pImageInfo[i].imageView == (VkImageView)(uintptr_t)(30 + i) && w->pImageInfo[i].imageLayout == VK_IMAGE_LAYOUT_GENERAL);
    } else if (captured == 2) {
        assert(w->descriptorCount == 3 && w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
        for (unsigned i = 0; i < 3; ++i) assert(w->pTexelBufferView[i] == (VkBufferView)(uintptr_t)(40 + i));
    } else {
        assert(captured == 3 && w->descriptorCount == 2 && w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
        for (unsigned i = 0; i < 2; ++i) assert(w->pBufferInfo[i].buffer == (VkBuffer)(uintptr_t)(50 + i) && w->pBufferInfo[i].offset == 64 * i && w->pBufferInfo[i].range == 128);
    }
    ++captured;
}
int main(void) {
    uint8_t bytes[300] = {0};
    VkDescriptorBufferInfo b = {(VkBuffer)17, 32, 96}; memcpy(bytes + 13, &b, sizeof b);
    for (unsigned i = 0; i < 2; ++i) {
        VkDescriptorImageInfo image = {(VkSampler)(uintptr_t)(20 + i), (VkImageView)(uintptr_t)(30 + i), VK_IMAGE_LAYOUT_GENERAL};
        memcpy(bytes + 61 + 29 * i, &image, sizeof image);
        b = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)(50 + i), 64 * i, 128}; memcpy(bytes + 193 + 27 * i, &b, sizeof b);
    }
    for (unsigned i = 0; i < 3; ++i) { VkBufferView view = (VkBufferView)(uintptr_t)(40 + i); memcpy(bytes + 143 + 11 * i, &view, sizeof view); }
    VkDescriptorUpdateTemplateEntry entries[] = {
        {0, 1, 9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 13, 0},
        {3, 1, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 61, 29},
        {6, 1, 3, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 143, 11},
        {9, 1, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 193, 27},
    };
    VkDescriptorUpdateTemplateCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount = 4, .pDescriptorUpdateEntries = entries};
    VkDescriptorUpdateTemplate t;
    assert(guest_vkCreateDescriptorUpdateTemplate(VK_NULL_HANDLE, &ci, NULL, &t) == VK_SUCCESS);
    // The object owns its entry layout; mutating the creation array is harmless.
    memset(entries, 0, sizeof entries);
    guest_vkUpdateDescriptorSetWithTemplate(VK_NULL_HANDLE, (VkDescriptorSet)9, t, bytes);
    assert(captured == 4); captured = 0;
    guest_vkUpdateDescriptorSetWithTemplate(VK_NULL_HANDLE, (VkDescriptorSet)9, t, bytes);
    assert(captured == 4); guest_vkDestroyDescriptorUpdateTemplate(VK_NULL_HANDLE, t, NULL);
    entries[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    assert(guest_vkCreateDescriptorUpdateTemplate(VK_NULL_HANDLE, &ci, NULL, &t) == VK_ERROR_FEATURE_NOT_PRESENT && !t);
    puts("guest Vulkan template layout: unaligned offsets, arbitrary and zero strides, typed arrays, consecutive-binding writes and owned entries passed");
}
