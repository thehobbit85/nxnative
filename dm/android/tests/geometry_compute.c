// Execute exported geometry kernels; stdout is the raw differential corpus.
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include "../../gfx/tests/geometry_corpus.h"
#include "geometry_bindings.h"
#define CHECK(call) do { VkResult r_ = (call); if (r_) { fprintf(stderr, "%s: %d\n", #call, r_); return 1; } } while (0)
static VkResult load_shader(VkDevice device, const char *directory, unsigned inv, unsigned topology, VkShaderModule *shader) {
    char path[1024]; int length = snprintf(path, sizeof path, "%s/export/00000000feed000%u.gc%u.spv", directory, inv, topology);
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
    if (argc != 2) { fprintf(stderr, "usage: geometry_compute fixture-directory > corpus.bin\n"); return 2; }
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
    const unsigned words[] = {GC_INPUT_WORDS, GC_VWORDS, GC_IWORDS, GC_INFO, GC_MAP};
    VkBuffer buffers[5]; VkDeviceMemory allocations[5]; uint32_t *mapped[5];
    for (unsigned i = 0; i < 5; i++) {
        VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = words[i] * 4,
            .usage = i == 4 ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT : i == 3 ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
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
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = sizes};
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
        CHECK(load_shader(device, argv[1], inv * 2 + 1, t, &shaders[inv][t]));
        VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shaders[inv][t], .pName = "main"}};
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL, &pipelines[inv][t]));
    }
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &cpci, NULL, &commands));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &cbai, &command));
    unsigned cases = 0; const unsigned counts[] = {0, 1, 33, 97};
    for (unsigned t = 0; t < 5; t++) for (unsigned inv = 0; inv < 2; inv++) for (unsigned inst = 1; inst <= 2; inst++)
    for (unsigned n = 0; n < 4; n++) for (unsigned base = 0; base < 2; base++) {
        geometry_case c = {t, inv * 2 + 1, inst, counts[n], base * 4096}; gc_fill(c, mapped);
        CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[inv][t]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        // Deliberately over-dispatch X/Y: the generated bounds checks own the guards.
        vkCmdDispatch(command, (c.primitives + 31) / 32 + 1, inst + 1, c.invocations);
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        if (gc_check(c, mapped[1], mapped[2], stdout)) return 1;
        cases++;
    }
    vkDestroyCommandPool(device, commands, NULL);
    for (unsigned inv = 0; inv < 2; inv++) for (unsigned t = 0; t < 5; t++) { vkDestroyPipeline(device, pipelines[inv][t], NULL); vkDestroyShaderModule(device, shaders[inv][t], NULL); }
    vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorPool(device, pool, NULL); vkDestroyDescriptorSetLayout(device, set_layout, NULL); vkDestroyBufferView(device, view, NULL);
    for (unsigned i = 0; i < 5; i++) { vkUnmapMemory(device, allocations[i]); vkDestroyBuffer(device, buffers[i], NULL); vkFreeMemory(device, allocations[i], NULL); }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
    fprintf(stderr, "Vulkan geometry corpus: %u cases passed\n", cases); return fflush(stdout) != 0;
}
