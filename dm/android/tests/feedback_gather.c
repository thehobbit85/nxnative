// Execute the production ordered feedback gather; stdout is the Metal comparison corpus.
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include "../../gfx/tests/feedback_corpus.h"
#include "../../gfx/nvn/vulkan/feedback_gather.h"
#define CHECK(call) do { VkResult r_ = (call); if (r_) { fprintf(stderr, "%s: %d\n", #call, r_); return 1; } } while (0)
static VkResult load_shader(VkDevice device, const char *directory, VkShaderModule *shader) {
    char path[1024]; int length = snprintf(path, sizeof path, "%s/feedback_gather.spv", directory);
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
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: feedback_gather fixture-directory > corpus.bin\n"); return 2; }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance; CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1; VkPhysicalDevice physical;
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
    const unsigned words[] = {FC_SOURCE_WORDS, FC_ORDER_WORDS, FC_OUTPUT_WORDS, sizeof(dm_vk_feedback_gather) / 4};
    VkBuffer buffers[4]; VkDeviceMemory allocations[4]; uint32_t *mapped[4];
    for (unsigned i = 0; i < 4; i++) {
        VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = words[i] * 4,
            .usage = i == 3 ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
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
    VkDescriptorSetLayoutBinding bindings[4];
    for (unsigned i = 0; i < 4; i++) bindings[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorCount = 1,
        .descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bindings};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &set_layout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo info[4]; VkWriteDescriptorSet writes[4];
    for (unsigned i = 0; i < 4; i++) {
        info[i] = (VkDescriptorBufferInfo){buffers[i], 0, words[i] * 4};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = bindings[i].descriptorType, .pBufferInfo = &info[i]};
    }
    vkUpdateDescriptorSets(device, 4, writes, 0, NULL);
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    VkShaderModule shader; CHECK(load_shader(device, argv[1], &shader));
    VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"}};
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &cpci, NULL, &commands));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &cbai, &command));
    fc_inputs(mapped[0], mapped[1]); unsigned cases = 0;
    for (unsigned l = 0; l < 2; l++) for (unsigned test = 0; test < FC_CASES + 11; test++) for (unsigned b = 0; b < 4; b++) {
        char text[2048]; fc_layout(l, text); dm_vk_feedback feedback = {0};
        for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n"))
            if (dm_vk_feedback_line(line, strlen(line), &feedback)) return 1;
        dm_vk_feedback_gather control = {.source_offset = FC_SOURCE_PREFIX, .source_records = FC_RECORDS,
            .order_offset = FC_ORDER_PREFIX, .count = test < FC_CASES ? fc_count(test) : FC_COUNT,
            .destination_offset = FC_PREFIX, .ordered = test < FC_CASES ? fc_ordered(test) : 1,
            .first = test < FC_CASES ? fc_first(test) : 0};
        int active = dm_vk_feedback_gather_layout(&feedback, b, &control);
        for (unsigned i = 0; i < 4; i++) info[i].range = words[i] * 4;
        unsigned negative = test - FC_CASES;
        if (test >= FC_CASES) switch (negative) {
            case 0: control.source_offset = UINT32_MAX; break;
            case 1: control.destination_offset = UINT32_MAX; break;
            case 2: control.order_offset = UINT32_MAX; break;
            case 3: control.first = UINT32_MAX; break;
            case 4: control.source_records = 0; break;
            case 5: control.stride = 513; break;
            case 6: control.ordered = 2; break;
            case 7: control.words[0] = 0; break;
            case 8: info[2].range = (FC_PREFIX + (active ? 2 * control.stride - 1 : 0)) * 4; break;
            case 9: info[1].range = (FC_ORDER_PREFIX + 3) * 4; break;
            case 10: info[0].range = (FC_SOURCE_PREFIX + 4 * FC_WORDS - 1) * 4; break;
        }
        memcpy(mapped[3], &control, sizeof control); memset(mapped[2], 0xcc, FC_OUTPUT_WORDS * 4);
        vkUpdateDescriptorSets(device, 4, writes, 0, NULL);
        CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        // An extra workgroup must not write beyond the requested records.
        if (active) vkCmdDispatch(command, (FC_COUNT + 63) / 64 + 1, 1, 1);
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        if (test < FC_CASES) {
            if (!fc_check(mapped[2], l, b, test)) return 1;
            if (fwrite(mapped[2], 4, FC_OUTPUT_WORDS, stdout) != FC_OUTPUT_WORDS) return 2;
        } else for (unsigned at = 0; at < FC_OUTPUT_WORDS; at++) {
            uint32_t expected = 0xcccccccc; unsigned stride = fc_stride(l, b);
            if (negative >= 8 && active && at >= FC_PREFIX && at - FC_PREFIX < FC_COUNT * stride) {
                unsigned linear = (at - FC_PREFIX) / stride, vertex = (linear * 17 + 3) % 43;
                if ((negative == 8 && linear < 1) || (negative == 9 && linear < 3) || (negative == 10 && vertex < 3))
                    expected = fc_bits(vertex, fc_word(l, b, (at - FC_PREFIX) % stride));
            }
            if (mapped[2][at] != expected) {
                fprintf(stderr, "negative%u layout%u buffer%u word%u: %08x != %08x\n", negative, l, b, at, mapped[2][at], expected);
                return 1;
            }
        }
        cases++;
    }
    vkDestroyCommandPool(device, commands, NULL); vkDestroyPipeline(device, pipeline, NULL); vkDestroyShaderModule(device, shader, NULL);
    vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorPool(device, pool, NULL); vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (unsigned i = 0; i < 4; i++) { vkUnmapMemory(device, allocations[i]); vkDestroyBuffer(device, buffers[i], NULL); vkFreeMemory(device, allocations[i], NULL); }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    fprintf(stderr, "Vulkan feedback corpus: %u cases passed\n", cases); return fflush(stdout) != 0;
}
