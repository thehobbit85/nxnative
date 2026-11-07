// Reuse the Apple frontend's template interpretation: retain the immutable
// entries and read each descriptor at offset + element * stride. Expand to
// ordinary native writes, preserving the full write for consecutive bindings.
// This also avoids the Android emulator ranchu template path: the public pixel
// fixture reads zero after a native template update, including through native
// device procedure pointers; the equivalent ordinary write renders correctly.
// The exact internal driver defect remains open; no descriptor is skipped.
#include "guest_vk.h"

typedef struct {
    uint32_t count;
    size_t scratch_size;
    VkDescriptorUpdateTemplateEntry entries[];
} guest_template;

static size_t descriptor_size(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return sizeof(VkDescriptorImageInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        return sizeof(VkDescriptorBufferInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return sizeof(VkBufferView);
    default: return 0; // Extension descriptor types are not advertised by this frontend.
    }
}
GVK(vkCreateDescriptorUpdateTemplate, GVK_DEVICE, VkResult, VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci,
    const VkAllocationCallbacks *allocation, VkDescriptorUpdateTemplate *out) {
    (void)device; (void)allocation; *out = VK_NULL_HANDLE;
    if (ci->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET || ci->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
    size_t bytes = 0;
    for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; ++i) {
        const VkDescriptorUpdateTemplateEntry *e = &ci->pDescriptorUpdateEntries[i];
        size_t size = descriptor_size(e->descriptorType);
        if (!size) return VK_ERROR_FEATURE_NOT_PRESENT;
        size_t n = (size_t)e->descriptorCount * size; if (n > bytes) bytes = n;
    }
    guest_template *t = malloc(sizeof *t + (size_t)ci->descriptorUpdateEntryCount * sizeof *t->entries);
    if (!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->count = ci->descriptorUpdateEntryCount; t->scratch_size = bytes;
    if (t->count) memcpy(t->entries, ci->pDescriptorUpdateEntries, t->count * sizeof *t->entries);
    *out = (VkDescriptorUpdateTemplate)t; return VK_SUCCESS;
}
GVK_ALIAS(vkCreateDescriptorUpdateTemplateKHR, vkCreateDescriptorUpdateTemplate, GVK_DEVICE);
GVK(vkDestroyDescriptorUpdateTemplate, GVK_DEVICE, void, VkDevice device, VkDescriptorUpdateTemplate object, const VkAllocationCallbacks *allocation) {
    (void)device; (void)allocation; free((guest_template *)object);
}
GVK_ALIAS(vkDestroyDescriptorUpdateTemplateKHR, vkDestroyDescriptorUpdateTemplate, GVK_DEVICE);
GVK(vkUpdateDescriptorSetWithTemplate, GVK_DEVICE, void, VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate object, const void *data) {
    const guest_template *t = (const guest_template *)object;
    union { VkDescriptorImageInfo image[8]; VkDescriptorBufferInfo buffer[8]; VkBufferView view[8]; } small;
    void *scratch = t->scratch_size <= sizeof small ? &small : malloc(t->scratch_size);
    if (!scratch) die("guest Vulkan descriptor template allocation failed");
    for (uint32_t i = 0; i < t->count; ++i) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
        if (!e->descriptorCount) continue;
        size_t size = descriptor_size(e->descriptorType);
        for (uint32_t j = 0; j < e->descriptorCount; ++j)
            memcpy((uint8_t *)scratch + j * size, (const uint8_t *)data + e->offset + j * e->stride, size);
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = e->dstBinding, .dstArrayElement = e->dstArrayElement,
            .descriptorCount = e->descriptorCount, .descriptorType = e->descriptorType};
        switch (e->descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            write.pImageInfo = scratch; break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            write.pTexelBufferView = scratch; break;
        default: write.pBufferInfo = scratch; break;
        }
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    }
    if (scratch != &small) free(scratch);
}
GVK_ALIAS(vkUpdateDescriptorSetWithTemplateKHR, vkUpdateDescriptorSetWithTemplate, GVK_DEVICE);
