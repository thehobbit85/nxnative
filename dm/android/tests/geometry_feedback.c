// Execute exported geometry -> reader capture -> ordered feedback in one GPU submission.
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include "../../gfx/tests/geometry_feedback_corpus.h"
#include "../../gfx/nvn/vulkan/feedback_gather.h"
#include "geometry_bindings.h"
#define CHECK(call) do { VkResult r_ = (call); if (r_) { fprintf(stderr, "%s: %d\n", #call, r_); return 1; } } while (0)
static VkResult load_shader(VkDevice device, const char *directory, const char *relative, VkShaderModule *shader) {
    char path[1024]; int length = snprintf(path, sizeof path, "%s/%s", directory, relative);
    if (length < 0 || (size_t)length >= sizeof path) return VK_ERROR_INITIALIZATION_FAILED;
    FILE *file = fopen(path, "rb"); if (!file) return VK_ERROR_INITIALIZATION_FAILED;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return VK_ERROR_INITIALIZATION_FAILED; }
    long size = ftell(file);
    if (size < 20 || size % 4 || fseek(file, 0, SEEK_SET)) { fclose(file); return VK_ERROR_INITIALIZATION_FAILED; }
    uint32_t *code = malloc((size_t)size); if (!code) { fclose(file); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    int read_ok = fread(code, 1, (size_t)size, file) == (size_t)size;
    if (fclose(file) || !read_ok) { free(code); return VK_ERROR_INITIALIZATION_FAILED; }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = code};
    VkResult result = vkCreateShaderModule(device, &ci, NULL, shader); free(code); return result;
}
static int reader_layout(const char *directory, unsigned inv, unsigned topology, uint32_t bindings[3], dm_vk_feedback *feedback) {
    char path[1024]; int n = snprintf(path, sizeof path, "%s/export/00000000feed000%u.gr%u.vc.vk.txt", directory, inv, topology);
    if (n < 0 || (size_t)n >= sizeof path) return 0;
    FILE *file = fopen(path, "r"); if (!file) return 0;
    char line[4096]; dm_vk_capture capture = {0}; dm_vk_geometry geometry = {0};
    int good = 1, captures = 0, geometries = 0; bindings[0] = UINT32_MAX;
    memset(feedback, 0, sizeof *feedback);
    while (fgets(line, sizeof line, file)) {
        size_t length = strlen(line); if (length && line[length - 1] == '\n') line[--length] = 0;
        if (!strncmp(line, "capture ", 8)) {
            if (captures++ || dm_vk_capture_line(line, length, &capture)) { good = 0; break; }
        } else if (!strncmp(line, "geometry ", 9)) {
            if (geometries++ || dm_vk_geometry_line(line, length, 0, &geometry)) { good = 0; break; }
        } else if (!strncmp(line, "tf ", 3)) {
            if (dm_vk_feedback_line(line, length, feedback)) { good = 0; break; }
        }
        unsigned binding, role;
        if (sscanf(line, "b %u ssbo 1 geometry %u", &binding, &role) == 2 && role == 1) bindings[0] = binding;
    }
    if (ferror(file)) good = 0;
    if (fclose(file)) good = 0;
    bindings[1] = capture.output_binding; bindings[2] = capture.control_binding;
    return good && captures == 1 && geometries == 1 && geometry.capture && geometry.topology == topology &&
           geometry.invocations == inv && geometry.vertices == 9 && geometry.stride == 134 && bindings[0] != UINT32_MAX &&
           feedback->stride[0] == 16 && feedback->count[0] == 1 && feedback->location[0][0] == 0 && feedback->components[0][0] == 4;
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: geometry_feedback fixture-directory > corpus.bin\n"); return 2; }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1; VkPhysicalDevice physical;
    VkResult picked = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (picked != VK_SUCCESS && picked != VK_INCOMPLETE) return 1;
    if (!count) return 77;
    VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures(physical, &features);
    if (!features.vertexPipelineStoresAndAtomics) return 77;
    features = (VkPhysicalDeviceFeatures){.vertexPipelineStoresAndAtomics = VK_TRUE};
    VkQueueFamilyProperties families[32]; count = 32; vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0; while (family < count && (families[family].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) != (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) family++;
    if (family == count) return 77;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &features};
    VkDevice device; CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const unsigned words[] = {GC_INPUT_WORDS, GC_VWORDS, GC_IWORDS, GC_INFO, GC_MAP, GF_CAPTURE_WORDS, 6, GF_ORDER_WORDS, GF_OUTPUT_WORDS, sizeof(dm_vk_feedback_gather) / 4};
    VkBuffer buffers[10]; VkDeviceMemory allocations[10]; uint32_t *mapped[10];
    for (unsigned i = 0; i < 10; i++) {
        VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = words[i] * 4,
            .usage = i == 4 ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT : (i == 3 || i == 6 || i == 9) ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        CHECK(vkCreateBuffer(device, &ci, NULL, &buffers[i]));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffers[i], &req);
        uint32_t type = 0;
        for (; type < memory.memoryTypeCount; type++)
            if ((req.memoryTypeBits & (1u << type)) && (memory.memoryTypes[type].propertyFlags &
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
        if (type == memory.memoryTypeCount) return 77;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        CHECK(vkAllocateMemory(device, &ai, NULL, &allocations[i])); CHECK(vkBindBufferMemory(device, buffers[i], allocations[i], 0));
        CHECK(vkMapMemory(device, allocations[i], 0, VK_WHOLE_SIZE, 0, (void **)&mapped[i]));
    }
    VkBufferViewCreateInfo bvci = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buffers[4], .format = VK_FORMAT_R32_UINT, .range = GC_MAP * 4};
    VkBufferView view; CHECK(vkCreateBufferView(device, &bvci, NULL, &view));
    VkDescriptorSetLayoutBinding bindings[5];
    for (unsigned i = 0; i < 5; i++) bindings[i] = (VkDescriptorSetLayoutBinding){.binding = gc_vk_bindings[i], .descriptorCount = 1,
        .descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = bindings};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &set_layout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 3, .poolSizeCount = 3, .pPoolSizes = sizes};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo info[5]; VkWriteDescriptorSet writes[5];
    for (unsigned i = 0; i < 5; i++) {
        info[i] = (VkDescriptorBufferInfo){buffers[i], 0, words[i] * 4};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = gc_vk_bindings[i],
            .descriptorCount = 1, .descriptorType = bindings[i].descriptorType, .pBufferInfo = i < 4 ? &info[i] : NULL, .pTexelBufferView = i == 4 ? &view : NULL};
    }
    vkUpdateDescriptorSets(device, 5, writes, 0, NULL);
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    VkPipeline pipelines[2][5]; VkShaderModule shaders[2][5];
    for (unsigned inv = 0; inv < 2; inv++) for (unsigned t = 0; t < 5; t++) {
        char relative[128]; snprintf(relative, sizeof relative, "export/00000000feed000%u.gc%u.spv", inv * 2 + 1, t);
        CHECK(load_shader(device, argv[1], relative, &shaders[inv][t]));
        VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shaders[inv][t], .pName = "main"}};
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL, &pipelines[inv][t]));
    }
    uint32_t capture_bindings[3]; dm_vk_feedback feedback;
    if (!reader_layout(argv[1], 1, 0, capture_bindings, &feedback)) return 1;
    VkDescriptorSetLayout helper_layouts[2]; VkPipelineLayout helper_pso_layouts[2]; VkDescriptorSet helper_sets[2];
    for (unsigned helper = 0; helper < 2; helper++) {
        unsigned count = helper ? 4 : 3;
        VkDescriptorSetLayoutBinding bindings[4]; VkDescriptorBufferInfo info[4]; VkWriteDescriptorSet writes[4];
        const unsigned roles[2][4] = {{1, 5, 6, 0}, {5, 7, 8, 9}};
        for (unsigned i = 0; i < count; i++) {
            bindings[i] = (VkDescriptorSetLayoutBinding){.binding = helper ? i : capture_bindings[i],
                .descriptorCount = 1, .descriptorType = i == count - 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stageFlags = helper ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_VERTEX_BIT};
            unsigned role = roles[helper][i]; info[i] = (VkDescriptorBufferInfo){buffers[role], 0, words[role] * 4};
        }
        VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = count, .pBindings = bindings};
        CHECK(vkCreateDescriptorSetLayout(device, &ci, NULL, &helper_layouts[helper]));
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
            .descriptorSetCount = 1, .pSetLayouts = &helper_layouts[helper]};
        CHECK(vkAllocateDescriptorSets(device, &ai, &helper_sets[helper]));
        for (unsigned i = 0; i < count; i++) writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = helper_sets[helper], .dstBinding = bindings[i].binding, .descriptorCount = 1,
            .descriptorType = bindings[i].descriptorType, .pBufferInfo = &info[i]};
        vkUpdateDescriptorSets(device, count, writes, 0, NULL);
        VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &helper_layouts[helper]};
        CHECK(vkCreatePipelineLayout(device, &pi, NULL, &helper_pso_layouts[helper]));
    }
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass renderpass; CHECK(vkCreateRenderPass(device, &rpci, NULL, &renderpass));
    VkFramebufferCreateInfo fbci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = renderpass, .width = 1, .height = 1, .layers = 1};
    VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(device, &fbci, NULL, &framebuffer));
    VkPipeline readers[2][5]; VkShaderModule reader_shaders[2][5];
    for (unsigned inv = 0; inv < 2; inv++) for (unsigned t = 0; t < 5; t++) {
        uint32_t bindings[3]; dm_vk_feedback current;
        if (!reader_layout(argv[1], inv * 2 + 1, t, bindings, &current) || memcmp(bindings, capture_bindings, sizeof bindings) ||
                memcmp(&current, &feedback, sizeof current)) return 1;
        char relative[128]; snprintf(relative, sizeof relative, "export/00000000feed000%u.gr%u.vc.spv", inv * 2 + 1, t);
        CHECK(load_shader(device, argv[1], relative, &reader_shaders[inv][t]));
        VkPipelineShaderStageCreateInfo stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = reader_shaders[inv][t], .pName = "main"};
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .rasterizerDiscardEnable = VK_TRUE, .lineWidth = 1};
        VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 1, .pStages = &stage,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pRasterizationState = &rs, .layout = helper_pso_layouts[0], .renderPass = renderpass};
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &readers[inv][t]));
    }
    VkShaderModule gather_shader; CHECK(load_shader(device, argv[1], "feedback_gather.spv", &gather_shader));
    VkComputePipelineCreateInfo gather_ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = helper_pso_layouts[1],
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = gather_shader, .pName = "main"}};
    VkPipeline gather; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &gather_ci, NULL, &gather));
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &cpci, NULL, &commands));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &cbai, &command));
    unsigned cases = 0; const unsigned counts[] = {0, 1, 33, 97};
    for (unsigned t = 0; t < 5; t++) for (unsigned inv = 0; inv < 2; inv++) for (unsigned inst = 1; inst <= 2; inst++)
    for (unsigned n = 0; n < 4; n++) for (unsigned base = 0; base < 2; base++) {
        geometry_case c = {t, inv * 2 + 1, inst, counts[n], base * 4096}; gc_fill(c, mapped); gf_fill(c, mapped[7], mapped[8]);
        unsigned records = gf_records(c); uint32_t capture_control[] = {0, records, 0, 1, GF_CAPTURE_PREFIX, records};
        memcpy(mapped[6], capture_control, sizeof capture_control); memset(mapped[5], 0xcc, GF_CAPTURE_WORDS * 4);
        dm_vk_feedback_gather gather_control = {.source_offset = GF_CAPTURE_PREFIX, .source_records = records,
            .order_offset = GF_ORDER_PREFIX, .count = records, .destination_offset = GF_PREFIX, .ordered = 1};
        if (!dm_vk_feedback_gather_layout(&feedback, 0, &gather_control)) return 1;
        memcpy(mapped[9], &gather_control, sizeof gather_control);
        CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[inv][t]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        // Deliberately over-dispatch X/Y: the generated bounds checks own the guards.
        vkCmdDispatch(command, (c.primitives + 31) / 32 + 1, inst + 1, c.invocations);
        VkMemoryBarrier transition = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &transition, 0, NULL, 0, NULL);
        VkRenderPassBeginInfo render = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = renderpass, .framebuffer = framebuffer,
            .renderArea.extent = {1, 1}};
        vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, readers[inv][t]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, helper_pso_layouts[0], 0, 1, &helper_sets[0], 0, NULL);
        vkCmdDraw(command, records + 3, 1, 0, 0); vkCmdEndRenderPass(command);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &transition, 0, NULL, 0, NULL);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, gather);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, helper_pso_layouts[1], 0, 1, &helper_sets[1], 0, NULL);
        vkCmdDispatch(command, (records + 63) / 64 + 1, 1, 1);
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        if (gc_check(c, mapped[1], mapped[2], NULL) || gf_check(c, mapped[8], stdout)) return 1;
        for (unsigned at = 0; at < GF_CAPTURE_WORDS; at++)
            if ((at < GF_CAPTURE_PREFIX || at >= GF_CAPTURE_PREFIX + records * 144) && mapped[5][at] != 0xcccccccc) {
                fprintf(stderr, "capture guard at word%u\n", at); return 1;
            }
        cases++;
    }
    vkDestroyCommandPool(device, commands, NULL);
    for (unsigned inv = 0; inv < 2; inv++) for (unsigned t = 0; t < 5; t++) { vkDestroyPipeline(device, pipelines[inv][t], NULL); vkDestroyShaderModule(device, shaders[inv][t], NULL); }
    for (unsigned inv = 0; inv < 2; inv++) for (unsigned t = 0; t < 5; t++) {
        vkDestroyPipeline(device, readers[inv][t], NULL); vkDestroyShaderModule(device, reader_shaders[inv][t], NULL);
    }
    vkDestroyPipeline(device, gather, NULL); vkDestroyShaderModule(device, gather_shader, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL); vkDestroyRenderPass(device, renderpass, NULL);
    for (unsigned i = 0; i < 2; i++) { vkDestroyPipelineLayout(device, helper_pso_layouts[i], NULL); vkDestroyDescriptorSetLayout(device, helper_layouts[i], NULL); }
    vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorPool(device, pool, NULL); vkDestroyDescriptorSetLayout(device, set_layout, NULL); vkDestroyBufferView(device, view, NULL);
    for (unsigned i = 0; i < 10; i++) { vkUnmapMemory(device, allocations[i]); vkDestroyBuffer(device, buffers[i], NULL); vkFreeMemory(device, allocations[i], NULL); }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    fprintf(stderr, "Vulkan geometry feedback corpus: %u cases passed\n", cases); return fflush(stdout) != 0;
}
