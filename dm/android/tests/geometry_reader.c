// Rasterize lowered geometry readers: viewport-zero masks and depth clamp.
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { VkResult r_ = (call); if (r_) { fprintf(stderr, "%s: %d\n", #call, r_); return 1; } } while (0)
enum { SIDE = 16, STRIDE = 134, VERTICES = 3, PIXELS = SIDE * SIDE };
static uint32_t bits(float value) { uint32_t word; memcpy(&word, &value, 4); return word; }
static VkResult module(VkDevice device, const char *path, VkShaderModule *shader) {
    FILE *file = fopen(path, "rb"); if (!file) return VK_ERROR_INITIALIZATION_FAILED;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return VK_ERROR_INITIALIZATION_FAILED; }
    long size = ftell(file);
    if (size < 20 || size % 4 || fseek(file, 0, SEEK_SET)) { fclose(file); return VK_ERROR_INITIALIZATION_FAILED; }
    uint32_t *code = malloc((size_t)size);
    if (!code) { fclose(file); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    int read_ok = fread(code, 1, (size_t)size, file) == (size_t)size;
    if (fclose(file) || !read_ok) { free(code); return VK_ERROR_INITIALIZATION_FAILED; }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = code};
    VkResult result = vkCreateShaderModule(device, &ci, NULL, shader); free(code); return result;
}
static uint32_t memory_type(VkPhysicalDeviceMemoryProperties *memory, uint32_t types, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory->memoryTypeCount; i++)
        if ((types & (1u << i)) && (memory->memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: geometry_reader vertex.spv pixel.spv\n"); return 2; }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1; VkPhysicalDevice physical;
    VkResult picked = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (picked != VK_SUCCESS && picked != VK_INCOMPLETE) return 1;
    if (!count) return 77;
    VkPhysicalDeviceFeatures available; vkGetPhysicalDeviceFeatures(physical, &available);
    if (!available.depthClamp) { fprintf(stderr, "requires depthClamp\n"); return 77; }
    VkPhysicalDeviceFeatures features = {.depthClamp = VK_TRUE};
    VkQueueFamilyProperties families[32]; count = 32; vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0; while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    if (family == count) return 77;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &features};
    VkDevice device; CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    VkBuffer buffers[2]; VkDeviceMemory allocations[2]; void *mapped[2];
    for (unsigned i = 0; i < 2; i++) {
        VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (i ? PIXELS : VERTICES * STRIDE) * 4,
                                .usage = i ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        CHECK(vkCreateBuffer(device, &ci, NULL, &buffers[i]));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffers[i], &req);
        uint32_t type = memory_type(&memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) return 77;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        CHECK(vkAllocateMemory(device, &ai, NULL, &allocations[i])); CHECK(vkBindBufferMemory(device, buffers[i], allocations[i], 0));
        CHECK(vkMapMemory(device, allocations[i], 0, VK_WHOLE_SIZE, 0, &mapped[i]));
    }
    VkImageCreateInfo image_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {SIDE, SIDE, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage image; CHECK(vkCreateImage(device, &image_ci, NULL, &image));
    VkMemoryRequirements image_req; vkGetImageMemoryRequirements(device, image, &image_req);
    uint32_t image_type = memory_type(&memory, image_req.memoryTypeBits, 0); if (image_type == UINT32_MAX) return 77;
    VkMemoryAllocateInfo image_ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = image_req.size, .memoryTypeIndex = image_type};
    VkDeviceMemory image_memory; CHECK(vkAllocateMemory(device, &image_ai, NULL, &image_memory)); CHECK(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_ci.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; CHECK(vkCreateImageView(device, &view_ci, NULL, &view));
    VkAttachmentDescription attachment = {.format = image_ci.format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkSubpassDependency dependency = {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 1, .pDependencies = &dependency};
    VkRenderPass renderpass; CHECK(vkCreateRenderPass(device, &rpci, NULL, &renderpass));
    VkFramebufferCreateInfo fbci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = renderpass, .attachmentCount = 1,
        .pAttachments = &view, .width = SIDE, .height = SIDE, .layers = 1};
    VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(device, &fbci, NULL, &framebuffer));
    VkDescriptorSetLayoutBinding binding = {.binding = 10000, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &set_layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo buffer_info = {buffers[0], 0, VERTICES * STRIDE * 4};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 10000,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer_info};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    VkShaderModule vertex, pixel; CHECK(module(device, argv[1], &vertex)); CHECK(module(device, argv[2], &pixel));
    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = pixel, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkViewport viewport = {0, 0, SIDE, SIDE, 0, 1}; VkRect2D scissor = {{0, 0}, {SIDE, SIDE}};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &blend, .layout = layout, .renderPass = renderpass};
    VkPipeline pipelines[2][2];
    for (unsigned clamp = 0; clamp < 2; clamp++) for (unsigned points = 0; points < 2; points++) {
        rs.depthClampEnable = clamp; ia.topology = points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipelines[clamp][points]));
    }
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &cpci, NULL, &commands));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &cbai, &command));
    const uint32_t masks[] = {0, 1, 2, 3, 0x80000000, 0xffffffff};
    for (unsigned clamp = 0; clamp < 2; clamp++) for (unsigned points = 0; points < 2; points++)
    for (unsigned far = 0; far < 2; far++) for (unsigned test = 0; test < 6; test++) {
        memset(mapped[0], 0, VERTICES * STRIDE * 4); memset(mapped[1], 0xcc, PIXELS * 4);
        for (unsigned v = 0; v < VERTICES; v++) {
            uint32_t *words = (uint32_t *)mapped[0] + v * STRIDE;
            words[0] = bits(points ? 0 : v == 1 ? 3 : -1); words[1] = bits(points ? 0 : v == 2 ? 3 : -1);
            words[2] = bits(far ? 2 : 0.5f); words[3] = words[4] = bits(1); words[5] = masks[test];
        }
        CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(command, &begin));
        VkClearValue clear = {.color = {{0, 0, 0, 1}}};
        VkRenderPassBeginInfo render = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = renderpass, .framebuffer = framebuffer,
            .renderArea.extent = {SIDE, SIDE}, .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[clamp][points]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
        vkCmdDraw(command, points ? 1 : 3, 1, 0, 0); vkCmdEndRenderPass(command);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {SIDE, SIDE, 1}};
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers[1], 1, &copy);
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        unsigned red = 0; uint8_t *pixels = mapped[1];
        for (unsigned p = 0; p < PIXELS; p++) {
            if (pixels[p * 4 + 1] || pixels[p * 4 + 2] || pixels[p * 4 + 3] != 255) return 1;
            red += pixels[p * 4] == 255;
        }
        unsigned expected = (masks[test] & 1) && (!far || clamp) ? (points ? 1 : PIXELS) : 0;
        if (red != expected) {
            fprintf(stderr, "mask=%08x points=%u clamp=%u far=%u: %u red pixels, expected %u\n", masks[test], points, clamp, far, red, expected); return 1;
        }
    }
    vkDestroyCommandPool(device, commands, NULL);
    for (unsigned clamp = 0; clamp < 2; clamp++) for (unsigned points = 0; points < 2; points++) vkDestroyPipeline(device, pipelines[clamp][points], NULL);
    vkDestroyShaderModule(device, vertex, NULL); vkDestroyShaderModule(device, pixel, NULL);
    vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorPool(device, pool, NULL); vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL); vkDestroyRenderPass(device, renderpass, NULL); vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, image, NULL); vkFreeMemory(device, image_memory, NULL);
    for (unsigned i = 0; i < 2; i++) { vkUnmapMemory(device, allocations[i]); vkDestroyBuffer(device, buffers[i], NULL); vkFreeMemory(device, allocations[i], NULL); }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    puts("Vulkan geometry reader: 48 mask, primitive and depth cases passed"); return 0;
}
