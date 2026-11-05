// Execute the offline vertex capture entry and verify the shared 144-word ABI.
// Input: synthetic fixture from test_android_vertex_capture.py --fixture FILE.
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { VkResult r_ = (call); if (r_) { fprintf(stderr, "%s: %d\n", #call, r_); return 1; } } while (0)
enum { WORDS = 144, PREFIX = 16, RECORDS = 8, TOTAL = PREFIX + RECORDS * WORDS + 16 };
static uint32_t bits(float value) { uint32_t result; memcpy(&result, &value, 4); return result; }

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: t_shader_capture capture.spv [capture.bin]\n"); return 2; }
    FILE *file = fopen(argv[1], "rb"); if (!file) return 2;
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    if (size < 20 || size % 4) return 2;
    uint32_t *code = malloc((size_t)size);
    if (!code || fread(code, 1, (size_t)size, file) != (size_t)size) return 2;
    fclose(file);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1; VkPhysicalDevice physical;
    VkResult picked = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (picked != VK_SUCCESS && picked != VK_INCOMPLETE) return 1;
    if (!count) return 77;
    VkPhysicalDeviceFeatures available; vkGetPhysicalDeviceFeatures(physical, &available);
    if (!available.vertexPipelineStoresAndAtomics || !available.shaderClipDistance) {
        fprintf(stderr, "requires vertex stores and clip-distance output\n"); return 77;
    }
    VkPhysicalDeviceFeatures features = {.vertexPipelineStoresAndAtomics = VK_TRUE, .shaderClipDistance = VK_TRUE};
    VkQueueFamilyProperties families[32]; count = 32; vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0; while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    if (family == count) return 77;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &features};
    VkDevice device; CHECK(vkCreateDevice(physical, &dci, NULL, &device)); VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    VkBuffer buffers[2]; VkDeviceMemory allocations[2]; void *mapped[2];
    for (unsigned i = 0; i < 2; i++) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = i ? 24 : TOTAL * 4,
                                 .usage = i ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
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
    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT}};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &set_layout));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass renderpass; CHECK(vkCreateRenderPass(device, &rpci, NULL, &renderpass));
    VkFramebufferCreateInfo fbci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = renderpass, .width = 1, .height = 1, .layers = 1};
    VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(device, &fbci, NULL, &framebuffer));
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = code};
    VkShaderModule shader; CHECK(vkCreateShaderModule(device, &smci, NULL, &shader)); free(code);
    VkPipelineShaderStageCreateInfo stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader, .pName = "main"};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .rasterizerDiscardEnable = VK_TRUE, .lineWidth = 1};
    VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 1, .pStages = &stage,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pRasterizationState = &rs, .layout = layout, .renderPass = renderpass};
    VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline));
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &cpci, NULL, &commands));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &cbai, &command));
    for (unsigned test = 0; test < 6; test++) {
        uint32_t control[] = {3, 4, 2, 2, PREFIX, test == 1 ? 4 : RECORDS};
        if (test == 3) control[4] = UINT32_MAX;
        if (test == 4) { control[0] = 0; control[1] = control[3] = control[5] = UINT32_MAX; control[2] = 0; }
        memcpy(mapped[1], control, sizeof control); memset(mapped[0], 0xcc, TOTAL * 4);
        VkDescriptorBufferInfo info[2] = {{buffers[0], 0, test == 5 ? (PREFIX + 2 * WORDS) * 4 : TOTAL * 4}, {buffers[1], 0, 24}};
        VkWriteDescriptorSet writes[2]; memset(writes, 0, sizeof writes);
        for (unsigned i = 0; i < 2; i++) writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = i, .descriptorCount = 1, .descriptorType = bindings[i].descriptorType, .pBufferInfo = &info[i]};
        vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
        CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(command, &begin));
        VkRenderPassBeginInfo render = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = renderpass, .framebuffer = framebuffer, .renderArea.extent = {1, 1}};
        vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
        vkCmdDraw(command, test == 2 ? 10 : 4, test == 2 ? 4 : 2, test == 2 ? 0 : 3, test == 2 ? 1 : 2);
        vkCmdEndRenderPass(command);
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        unsigned records = test == 1 ? 4 : test == 3 || test == 4 ? 0 : test == 5 ? 2 : RECORDS;
        for (unsigned offset = 0; offset < TOTAL; offset++) {
            uint32_t expected = 0xcccccccc;
            if (offset >= PREFIX && offset - PREFIX < records * WORDS) {
                unsigned record = (offset - PREFIX) / WORDS, word = (offset - PREFIX) % WORDS;
                expected = word == 3 || word == 4 || (word >= 19 && word % 4 == 3) ? bits(1) : 0;
                if (word == 4) expected = bits(64);
                if (word == 5) expected = bits(-0.5f);
                if (word == 12) expected = bits(0.5f);
                if (word >= 16 && word < 20) expected = bits((float)(3 + record % 4 + (word - 16) * 10));
                if (word == 44) expected = bits(42);
                if (word >= 136 && word < 140) expected = word == 136 ? 42 : 0;
                if (word >= 140) { uint32_t raw[] = {1, 0xdeadbeef, 0x7fc01234, 2 + record / 4}; expected = raw[word - 140]; }
            }
            uint32_t actual = ((uint32_t *)mapped[0])[offset];
            if (actual != expected) { fprintf(stderr, "case %u word %u: %#x, expected %#x\n", test, offset, actual, expected); return 1; }
        }
        if (!test && argc == 3) {
            FILE *output = fopen(argv[2], "wb");
            if (!output) return 2;
            int written = fwrite(mapped[0], 4, TOTAL, output) == TOTAL;
            if (fclose(output) || !written) return 2;
        }
    }
    vkDestroyCommandPool(device, commands, NULL); vkDestroyPipeline(device, pipeline, NULL); vkDestroyShaderModule(device, shader, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL); vkDestroyRenderPass(device, renderpass, NULL); vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorPool(device, pool, NULL); vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (unsigned i = 0; i < 2; i++) { vkUnmapMemory(device, allocations[i]); vkDestroyBuffer(device, buffers[i], NULL); vkFreeMemory(device, allocations[i], NULL); }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    puts("Vulkan vertex capture: 6 raw-word, range and overflow cases passed"); return 0;
}
