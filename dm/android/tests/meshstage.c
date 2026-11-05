// Execute task/mesh stage-memory exports, including bounded input/output ranges.
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../gfx/nvn/vulkan/shader_contract.h"
#define INPUT_WORDS 400u
#define OUTPUT_WORDS (13u + 35u * 424u + 17u)
#define CHECK(call) do { VkResult r_ = (call); if (r_) { fprintf(stderr, "%s: %d\n", #call, r_); return 1; } } while (0)
static VkResult load_shader(VkDevice device, const char *directory, unsigned variant, VkShaderModule *shader, dm_vk_meshstage *mesh) {
    char path[1024], text[8192];
    int length = snprintf(path, sizeof path, "%s/export/00000000feed001%u.vk.txt", directory, variant);
    if (length < 0 || (size_t)length >= sizeof path) return VK_ERROR_INITIALIZATION_FAILED;
    FILE *file = fopen(path, "rb"); if (!file) return VK_ERROR_INITIALIZATION_FAILED;
    size_t text_size = fread(text, 1, sizeof text - 1, file); int text_ok = feof(file) && !ferror(file); fclose(file);
    if (!text_ok) return VK_ERROR_INITIALIZATION_FAILED;
    text[text_size] = 0;
    snprintf(path, sizeof path, "%s/export/00000000feed001%u.spv", directory, variant);
    file = fopen(path, "rb"); if (!file) return VK_ERROR_INITIALIZATION_FAILED;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return VK_ERROR_INITIALIZATION_FAILED; }
    long size = ftell(file);
    if (size < 20 || size % 4 || fseek(file, 0, SEEK_SET)) { fclose(file); return VK_ERROR_INITIALIZATION_FAILED; }
    uint32_t *code = malloc((size_t)size); if (!code) { fclose(file); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    int read_ok = fread(code, 1, (size_t)size, file) == (size_t)size;
    if (fclose(file) || !read_ok) { free(code); return VK_ERROR_INITIALIZATION_FAILED; }
    const char *error = dm_vk_shader_contract(text, text_size, code, (size_t)size / 4);
    if (error) { fprintf(stderr, "%s\n", error); free(code); return VK_ERROR_INITIALIZATION_FAILED; }
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "mesh ", 5)) error = dm_vk_meshstage_line(line, strlen(line), mesh);
        else if (!strncmp(line, "meshv ", 6) || !strncmp(line, "meshp ", 6)) error = dm_vk_meshstage_map(line, strlen(line), line[4] == 'v', mesh);
        if (error) { fprintf(stderr, "%s\n", error); free(code); return VK_ERROR_INITIALIZATION_FAILED; }
    }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = code};
    VkResult result = vkCreateShaderModule(device, &ci, NULL, shader); free(code); return result;
}
static uint32_t bits(unsigned n) {
    const uint32_t values[] = {0x80000000, 0x7fc12345, 1, 0xff800000, 0x3f800000, 0xdeadbeef, 0x007fffff, 0};
    return values[n % 8];
}
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: meshstage fixture-directory variant\n"); return 2; }
    unsigned variant = (unsigned)strtoul(argv[2], NULL, 10); if (variant > 4) return 2;
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
    VkShaderModule shader; dm_vk_meshstage mesh = {0}; CHECK(load_shader(device, argv[1], variant, &shader, &mesh));
    const unsigned words[] = {INPUT_WORDS, OUTPUT_WORDS, 8};
    const uint32_t slots[] = {mesh.input_binding, mesh.output_binding, mesh.control_binding};
    VkBuffer buffers[3]; VkDeviceMemory allocations[3]; uint32_t *mapped[3];
    for (unsigned i = 0; i < 3; i++) {
        VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = words[i] * 4,
            .usage = i == 2 ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
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
    VkDescriptorSetLayoutBinding bindings[3];
    for (unsigned i = 0; i < 3; i++) bindings[i] = (VkDescriptorSetLayoutBinding){.binding = slots[i], .descriptorCount = 1,
        .descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &set_layout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo info[3]; VkWriteDescriptorSet writes[3];
    for (unsigned i = 0; i < 3; i++) {
        info[i] = (VkDescriptorBufferInfo){buffers[i], 0, words[i] * 4};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = slots[i],
            .descriptorCount = 1, .descriptorType = bindings[i].descriptorType, .pBufferInfo = &info[i]};
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"}};
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &cpci, NULL, &commands));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &cbai, &command));
    for (unsigned i = 0; i < INPUT_WORDS; i++) mapped[0][i] = bits(i);
    for (unsigned g = 0; g < 35; g++) mapped[0][5 + g * 11] = g * 17 + 9;
    unsigned cases = 0; const unsigned counts[] = {0, 1, 3, 33};
    const uint32_t firsts[] = {0, 4096, UINT32_MAX - 1};
    for (unsigned n = 0; n < 4; n++) for (unsigned f = 0; f < 3; f++) for (unsigned test = 0; test < 11; test++) {
        uint32_t ctl[8] = {firsts[f], counts[n], 13, 35, 5, 35, 11, 0};
        unsigned valid = counts[n];
        info[0].range = INPUT_WORDS * 4; info[1].range = OUTPUT_WORDS * 4;
        switch (test) {
            case 1: ctl[3] = 0; valid = 0; break;
            case 2: ctl[2] = UINT32_MAX; valid = 0; break;
            case 3: ctl[7] = 1; valid = 0; break;
            case 4: ctl[5] = 0; if (variant == 3) valid = 0; break;
            case 5: ctl[4] = UINT32_MAX; if (variant == 3) valid = 0; break;
            case 6: ctl[6] = 0; if (variant == 3) valid = 0; break;
            case 7: info[1].range = (13 + 2 * mesh.words - 1) * 4; if (valid > 1) valid = 1; break;
            case 8: info[0].range = (5 + 11 + 9 - 1) * 4; if (variant == 3 && valid > 1) valid = 1; break;
            case 9: ctl[6] = 8; if (variant == 3) valid = 0; break;
            case 10: ctl[3] = 2; if (valid > 2) valid = 2; break;
        }
        memcpy(mapped[2], ctl, sizeof ctl); memset(mapped[1], 0xcc, OUTPUT_WORDS * 4);
        vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
        CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        vkCmdDispatch(command, counts[n] + 2, 2, 2);
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        for (unsigned at = 0; at < OUTPUT_WORDS; at++) {
            uint32_t expected = 0xcccccccc;
            if (at >= 13 && at - 13 < valid * mesh.words) {
                unsigned g = (at - 13) / mesh.words, w = (at - 13) % mesh.words;
                uint32_t number = variant == 3 ? g * 17 + 9 : firsts[f] + g;
                expected = 0;
                if (w == 0) expected = number;
                if (variant == 3 && w >= 4 && w < 12) expected = w == 4 ? number : bits(5 + g * 11 + 1 + w - 4);
                if (variant == 4) { if (w == 16) expected = number; }
                else if (variant < 2) {
                    if (w == 12) expected = number;
                    if (w == 13) expected = 2048;
                    if (w == 14) expected = 0x7fc12345;
                } else {
                    if (w == 12) expected = number;
                    if (w == 44) expected = 0x7fc12345;
                    if (w == 76) expected = variant == 3 ? number : 0;
                    if (w == 108) expected = variant == 3 ? bits(5 + g * 11 + 2) : 0;
                    if (w == 140) expected = variant == 3 ? number : g;
                    if (w == 396) expected = 0x23000000;
                    if (w == 397) expected = 0x03020100;
                }
            }
            if (mapped[1][at] != expected) {
                fprintf(stderr, "variant%u count%u first%u test%u word%u: %08x != %08x\n", variant, counts[n], firsts[f], test, at, mapped[1][at], expected);
                return 1;
            }
        }
        cases++;
    }
    vkDestroyCommandPool(device, commands, NULL); vkDestroyPipeline(device, pipeline, NULL); vkDestroyShaderModule(device, shader, NULL);
    vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorPool(device, pool, NULL); vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (unsigned i = 0; i < 3; i++) { vkUnmapMemory(device, allocations[i]); vkDestroyBuffer(device, buffers[i], NULL); vkFreeMemory(device, allocations[i], NULL); }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    fprintf(stderr, "Vulkan task/mesh variant%u: %u cases passed\n", variant, cases); return 0;
}
