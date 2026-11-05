// Distinguish raw topology indices from unrelated floating-point texel views.
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { VkResult r_ = (call); if (r_) { fprintf(stderr, "%s: %d\n", #call, r_); return 1; } } while (0)
enum { N = 16, OUTPUT = N * 3 + 4 };
static uint32_t float_bits(float value) { uint32_t bits; memcpy(&bits, &value, 4); return bits; }

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "float-view"))) {
        fprintf(stderr, "usage: t_geometry_topology shader.spv [float-view]\n"); return 2;
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
    VkPhysicalDevice physical; uint32_t count = 1;
    VkResult picked = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (picked != VK_SUCCESS && picked != VK_INCOMPLETE) return 1;
    if (!count) return 77;
    VkQueueFamilyProperties families[32]; count = 32; vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0; while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
    if (family == count) return 77;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice device; CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    VkBuffer buffers[3]; VkDeviceMemory allocations[3]; void *mapped[3];
    for (unsigned i = 0; i < 3; i++) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (i == 2 ? OUTPUT : N) * 4,
                                 .usage = i == 2 ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT};
        CHECK(vkCreateBuffer(device, &bci, NULL, &buffers[i]));
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        uint32_t type = 0;
        for (; type < memory.memoryTypeCount; type++)
            if ((requirements.memoryTypeBits & (1u << type)) && (memory.memoryTypes[type].propertyFlags &
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
        if (type == memory.memoryTypeCount) return 77;
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
        CHECK(vkAllocateMemory(device, &mai, NULL, &allocations[i])); CHECK(vkBindBufferMemory(device, buffers[i], allocations[i], 0));
        CHECK(vkMapMemory(device, allocations[i], 0, VK_WHOLE_SIZE, 0, &mapped[i]));
    }
    const uint32_t indices[N] = {0, 1, 2, 3, 17, 255, 65535, 0x007fffff, 0x00800000,
                                 0x80000000, 0x80000001, 0xffffffff, 0x7fc01234, 0x7f800000, 0xff800000, 0xdeadbeef};
    memcpy(mapped[0], indices, sizeof indices);
    for (unsigned i = 0; i < N; i++) ((uint32_t *)mapped[1])[i] = float_bits((float)i + 1);
    for (unsigned i = 0; i < OUTPUT; i++) ((uint32_t *)mapped[2])[i] = 0xcafebabe;
    VkBufferView views[2];
    for (unsigned i = 0; i < 2; i++) {
        VkBufferViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buffers[i],
                                    .format = i || argc == 3 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32_UINT, .range = N * 4};
        CHECK(vkCreateBufferView(device, &ci, NULL, &views[i]));
    }
    VkDescriptorSetLayoutBinding bindings[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &set_layout));
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = pool_sizes};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo output = {buffers[2], 0, OUTPUT * 4};
    VkWriteDescriptorSet writes[3];
    for (unsigned i = 0; i < 3; i++) writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = i, .descriptorCount = 1, .descriptorType = bindings[i].descriptorType,
        .pBufferInfo = i == 2 ? &output : NULL, .pTexelBufferView = i < 2 ? &views[i] : NULL};
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    FILE *file = fopen(argv[1], "rb"); if (!file) return 2;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 2; }
    long size = ftell(file);
    if (size < 20 || size % 4) { fclose(file); return 2; }
    if (fseek(file, 0, SEEK_SET)) { fclose(file); return 2; }
    uint32_t *code = malloc((size_t)size);
    if (!code) { fclose(file); return 2; }
    int read_ok = fread(code, 1, (size_t)size, file) == (size_t)size;
    if (fclose(file) || !read_ok) { free(code); return 2; }
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = code};
    VkShaderModule shader; VkResult module_result = vkCreateShaderModule(device, &smci, NULL, &shader);
    free(code); CHECK(module_result);
    VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"}};
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &cpci, NULL, &commands));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &cbai, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(command, N, 1, 1);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    uint32_t *actual = mapped[2]; unsigned failures = 0;
    for (unsigned i = 0; i < OUTPUT; i++) {
        uint32_t expected = i >= N * 3 ? 0xcafebabe : i % 3 == 0 ? indices[i / 3] : i % 3 == 1 ? float_bits((float)(i / 3) + 1) : N;
        if (actual[i] != expected) { fprintf(stderr, "word %u: %08x != %08x\n", i, actual[i], expected); failures++; }
    }
    vkDestroyCommandPool(device, commands, NULL); vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, shader, NULL); vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorPool(device, pool, NULL); vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (unsigned i = 0; i < 2; i++) vkDestroyBufferView(device, views[i], NULL);
    for (unsigned i = 0; i < 3; i++) { vkUnmapMemory(device, allocations[i]); vkDestroyBuffer(device, buffers[i], NULL); vkFreeMemory(device, allocations[i], NULL); }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    if (failures) return 1;
    puts("Vulkan geometry topology: 16 raw indices, independent float view, size query and output guards passed");
    return 0;
}
