// Run one guest image atomic and compare every texel in all three array layers.
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { VkResult r = (call); if (r) { fprintf(stderr, "%s: %d\n", #call, r); return 1; } } while (0)
enum { WIDTH = 8, HEIGHT = 4, LAYERS = 3, BYTES = WIDTH * HEIGHT * LAYERS * sizeof(uint32_t) };

static void *read_file(const char *key, const char *ext, size_t *bytes) {
    char path[96];
    int n = snprintf(path, sizeof path, "%s.%s", key, ext);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    long size = -1;
    if (!fseek(file, 0, SEEK_END)) size = ftell(file);
    if (size <= 0 || size > 4 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    void *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) { free(data); data = NULL; }
    fclose(file);
    *bytes = (size_t)size;
    return data;
}

static VkResult allocate(VkDevice d, const VkPhysicalDeviceMemoryProperties *mem, VkMemoryRequirements req,
                         VkMemoryPropertyFlags flags, VkDeviceMemory *out) {
    for (uint32_t i = 0; i < mem->memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) && (mem->memoryTypes[i].propertyFlags & flags) == flags) {
            VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=req.size, .memoryTypeIndex=i};
            return vkAllocateMemory(d, &ai, NULL, out);
        }
    }
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static int compare(const char *key, uint32_t slot, const uint32_t *code, size_t bytes,
                   const void *initial, const void *expected) {
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app};
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    VkPhysicalDevice gpu;
    uint32_t count = 1;
    VkResult picked = vkEnumeratePhysicalDevices(instance, &count, &gpu);
    if ((picked != VK_SUCCESS && picked != VK_INCOMPLETE) || !count) return 77;
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R32_UINT, &format);
    if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT)) return 77;
    VkQueueFamilyProperties families[32];
    count = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
    if (family == count) return 77;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex=family,
        .queueCount=1, .pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(gpu, &features);
    VkDeviceCreateInfo dci = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pEnabledFeatures=&features,
        .queueCreateInfoCount=1, .pQueueCreateInfos=&qci};
    VkDevice dev;
    CHECK(vkCreateDevice(gpu, &dci, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);

    VkExtent3D extent = {WIDTH, HEIGHT, 1};
    VkImageCreateInfo imci = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R32_UINT, .extent=extent, .mipLevels=1, .arrayLayers=LAYERS, .samples=VK_SAMPLE_COUNT_1_BIT,
        .usage=VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage im;
    CHECK(vkCreateImage(dev, &imci, NULL, &im));
    VkMemoryRequirements req;
    VkDeviceMemory im_mem, buf_mem;
    vkGetImageMemoryRequirements(dev, im, &req);
    CHECK(allocate(dev, &mem, req, 0, &im_mem));
    CHECK(vkBindImageMemory(dev, im, im_mem, 0));
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS};
    VkImageViewCreateInfo vci = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=im, .format=imci.format,
        .viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY, .subresourceRange=range};
    VkImageView view;
    CHECK(vkCreateImageView(dev, &vci, NULL, &view));
    VkBufferCreateInfo bci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=BYTES,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer staging;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &staging));
    vkGetBufferMemoryRequirements(dev, staging, &req);
    CHECK(allocate(dev, &mem, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf_mem));
    CHECK(vkBindBufferMemory(dev, staging, buf_mem, 0));
    void *mapped = NULL;
    CHECK(vkMapMemory(dev, buf_mem, 0, BYTES, 0, &mapped));
    memcpy(mapped, initial, BYTES);

    VkDescriptorSetLayoutBinding binding = {.binding=slot,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo lci = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount=1, .pBindings=&binding};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &set_layout));
    VkPipelineLayoutCreateInfo plci = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&set_layout};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
    VkShaderModuleCreateInfo sci = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .pCode=code, .codeSize=bytes};
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(dev, &sci, NULL, &shader));
    VkComputePipelineCreateInfo pci = {.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout=layout,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_COMPUTE_BIT, .module=shader, .pName="main"}};
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
    VkDescriptorPoolSize ps = {binding.descriptorType, 1};
    VkDescriptorPoolCreateInfo dpci = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets=1, .poolSizeCount=1, .pPoolSizes=&ps};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool=pool,
        .descriptorSetCount=1, .pSetLayouts=&set_layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
    VkDescriptorImageInfo im_info = {.imageView=view, .imageLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=set, .dstBinding=binding.binding,
        .descriptorCount=1, .descriptorType=binding.descriptorType, .pImageInfo=&im_info};
    vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);

    VkCommandPoolCreateInfo cpci = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=family};
    VkCommandPool cmd_pool;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cbai = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=cmd_pool, .commandBufferCount=1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    VkImageMemoryBarrier bar = {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image=im, .subresourceRange=range,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED, .newLayout=VK_IMAGE_LAYOUT_GENERAL, .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    VkBufferImageCopy copy = {.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS}, .imageExtent=extent};
    vkCmdCopyBufferToImage(cmd, staging, im, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    bar.oldLayout = bar.newLayout;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    vkCmdCopyImageToBuffer(cmd, im, VK_IMAGE_LAYOUT_GENERAL, staging, 1, &copy);
    VkMemoryBarrier host = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount=1, .pCommandBuffers=&cmd};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    int mismatch = memcmp(mapped, expected, BYTES) != 0;
    if (mismatch) {
        for (size_t i=0; i<BYTES/4; i++) if (((uint32_t *)mapped)[i] != ((uint32_t *)expected)[i]) {
            fprintf(stderr, "%s texel %zu: got %u, expected %u\n", key, i, ((uint32_t *)mapped)[i], ((uint32_t *)expected)[i]); break;
        }
    }
    vkUnmapMemory(dev, buf_mem);
    vkDestroyCommandPool(dev, cmd_pool, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL);
    vkDestroyPipeline(dev, pipeline, NULL);
    vkDestroyShaderModule(dev, shader, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, set_layout, NULL);
    vkDestroyBuffer(dev, staging, NULL);
    vkDestroyImageView(dev, view, NULL);
    vkDestroyImage(dev, im, NULL);
    vkFreeMemory(dev, buf_mem, NULL);
    vkFreeMemory(dev, im_mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    return mismatch;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    size_t init_size = 0, expected_size = 0, bytes = 0;
    void *initial = read_file(argv[1], "initial", &init_size);
    void *expected = read_file(argv[1], "expected", &expected_size);
    uint32_t *code = read_file(argv[1], "spv", &bytes);
    int result = 1;
    if (initial && expected && code && init_size == BYTES && expected_size == BYTES && bytes >= 20 && bytes % 4 == 0) {
        result = compare(argv[1], (uint32_t)strtoul(argv[2], NULL, 10), code, bytes, initial, expected);
    }
    free(code);
    free(initial);
    free(expected);
    return result;
}
