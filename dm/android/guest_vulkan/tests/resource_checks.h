// Native objects created through guest procedures, including native SPIR-V compilation.
static const uint32_t triangle_vert[] =
#include "triangle.vert.h"
;
static const uint32_t triangle_frag[] =
#include "triangle.frag.h"
;
#define LOAD(name) PFN_##name name = (void *)gdpa(device, #name); assert(name)
#define OK(call) assert((call) == VK_SUCCESS)
static uint32_t memory_type(VkPhysicalDeviceMemoryProperties *p, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < p->memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (p->memoryTypes[i].propertyFlags & flags) == flags) return i;
    assert(!"missing required memory type"); return 0;
}
static void resource_checks(VkInstance instance, VkPhysicalDevice pd, VkDevice device, PFN_vkGetDeviceProcAddr gdpa) {
    LOAD(vkCreateBuffer); LOAD(vkDestroyBuffer); LOAD(vkGetBufferMemoryRequirements2); LOAD(vkBindBufferMemory2);
    LOAD(vkAllocateMemory); LOAD(vkFreeMemory); LOAD(vkMapMemory); LOAD(vkUnmapMemory);
    LOAD(vkFlushMappedMemoryRanges); LOAD(vkInvalidateMappedMemoryRanges);
    LOAD(vkCreateImage); LOAD(vkDestroyImage); LOAD(vkGetImageMemoryRequirements2); LOAD(vkBindImageMemory2);
    LOAD(vkCreateImageView); LOAD(vkDestroyImageView); LOAD(vkCreateSampler); LOAD(vkDestroySampler);
    LOAD(vkCreateDescriptorSetLayout); LOAD(vkDestroyDescriptorSetLayout); LOAD(vkCreateDescriptorPool); LOAD(vkDestroyDescriptorPool);
    LOAD(vkAllocateDescriptorSets); LOAD(vkUpdateDescriptorSets); LOAD(vkFreeDescriptorSets); LOAD(vkResetDescriptorPool);
    LOAD(vkCreateDescriptorUpdateTemplate); LOAD(vkUpdateDescriptorSetWithTemplate); LOAD(vkDestroyDescriptorUpdateTemplate);
    LOAD(vkCreateRenderPass); LOAD(vkDestroyRenderPass); LOAD(vkCreateFramebuffer); LOAD(vkDestroyFramebuffer);
    LOAD(vkCreatePipelineLayout); LOAD(vkDestroyPipelineLayout); LOAD(vkCreateShaderModule); LOAD(vkDestroyShaderModule);
    LOAD(vkCreatePipelineCache); LOAD(vkDestroyPipelineCache); LOAD(vkGetPipelineCacheData); LOAD(vkMergePipelineCaches);
    LOAD(vkCreateGraphicsPipelines); LOAD(vkDestroyPipeline);
    assert(gdpa(device, "vkGetBufferMemoryRequirements2KHR") == (PFN_vkVoidFunction)vkGetBufferMemoryRequirements2);
    assert(gdpa(device, "vkBindImageMemory2KHR") == (PFN_vkVoidFunction)vkBindImageMemory2);
    assert(gdpa(device, "vkCreateDescriptorUpdateTemplateKHR") == (PFN_vkVoidFunction)vkCreateDescriptorUpdateTemplate);
    PFN_vkGetPhysicalDeviceMemoryProperties properties = (void *)gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties memory; properties(pd, &memory);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer buffer; OK(vkCreateBuffer(device, &bi, NULL, &buffer));
    VkBufferMemoryRequirementsInfo2 br = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = buffer};
    VkMemoryDedicatedRequirements dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 mr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    vkGetBufferMemoryRequirements2(device, &br, &mr); assert(mr.memoryRequirements.size >= bi.size);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.memoryRequirements.size,
        .memoryTypeIndex = memory_type(&memory, mr.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    VkDeviceMemory bm; OK(vkAllocateMemory(device, &ma, NULL, &bm));
    VkBindBufferMemoryInfo bb = {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, .buffer = buffer, .memory = bm};
    OK(vkBindBufferMemory2(device, 1, &bb));
    uint32_t *mapped; OK(vkMapMemory(device, bm, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    for (unsigned i = 0; i < 1024; ++i) mapped[i] = i ^ 0x12345678u;
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = bm, .size = VK_WHOLE_SIZE};
    OK(vkFlushMappedMemoryRanges(device, 1, &range)); vkUnmapMemory(device, bm);
    OK(vkMapMemory(device, bm, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    OK(vkInvalidateMappedMemoryRanges(device, 1, &range));
    for (unsigned i = 0; i < 1024; ++i) assert(mapped[i] == (i ^ 0x12345678u));
    vkUnmapMemory(device, bm);

    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImage image; OK(vkCreateImage(device, &ii, NULL, &image));
    VkImageMemoryRequirementsInfo2 ir = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = image};
    vkGetImageMemoryRequirements2(device, &ir, &mr);
    ma.allocationSize = mr.memoryRequirements.size; ma.memoryTypeIndex = memory_type(&memory, mr.memoryRequirements.memoryTypeBits, 0);
    VkDeviceMemory im; OK(vkAllocateMemory(device, &ma, NULL, &im));
    VkBindImageMemoryInfo ib = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .image = image, .memory = im};
    OK(vkBindImageMemory2(device, 1, &ib));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = ii.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; OK(vkCreateImageView(device, &vi, NULL, &view));
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .maxLod = 1};
    VkSampler sampler; OK(vkCreateSampler(device, &si, NULL, &sampler));
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout layout; OK(vkCreateDescriptorSetLayout(device, &li, NULL, &layout));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorPool pool; OK(vkCreateDescriptorPool(device, &pi, NULL, &pool));
    VkDescriptorSetAllocateInfo sa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &layout};
    VkDescriptorSet set; OK(vkAllocateDescriptorSets(device, &sa, &set));
    VkDescriptorBufferInfo db = {buffer, 0, 256};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
        .descriptorType = binding.descriptorType, .pBufferInfo = &db};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkDescriptorUpdateTemplateEntry entry = {.descriptorCount = 1, .descriptorType = binding.descriptorType, .stride = sizeof db};
    VkDescriptorUpdateTemplateCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, .descriptorUpdateEntryCount = 1,
        .pDescriptorUpdateEntries = &entry, .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, .descriptorSetLayout = layout};
    VkDescriptorUpdateTemplate update; OK(vkCreateDescriptorUpdateTemplate(device, &ti, NULL, &update));
    vkUpdateDescriptorSetWithTemplate(device, set, update, &db);

    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layout};
    VkPipelineLayout pipeline_layout; OK(vkCreatePipelineLayout(device, &pl, NULL, &pipeline_layout));
    VkAttachmentDescription attachment = {.format = ii.format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass pass; OK(vkCreateRenderPass(device, &rp, NULL, &pass));
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass, .attachmentCount = 1,
        .pAttachments = &view, .width = 16, .height = 16, .layers = 1};
    VkFramebuffer framebuffer; OK(vkCreateFramebuffer(device, &fi, NULL, &framebuffer));
    VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof triangle_vert, .pCode = triangle_vert};
    VkShaderModule vertex, fragment; OK(vkCreateShaderModule(device, &sm, NULL, &vertex));
    sm.codeSize = sizeof triangle_frag; sm.pCode = triangle_frag; OK(vkCreateShaderModule(device, &sm, NULL, &fragment));
    VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex_input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, 16, 16, 0, 1}; VkRect2D scissor = {{0, 0}, {16, 16}};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState color = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &color};
    VkGraphicsPipelineCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly, .pViewportState = &vp, .pRasterizationState = &raster,
        .pMultisampleState = &ms, .pColorBlendState = &blend, .layout = pipeline_layout, .renderPass = pass};
    VkPipelineCacheCreateInfo cache_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache, cache2; OK(vkCreatePipelineCache(device, &cache_info, NULL, &cache));
    VkPipeline pipeline; OK(vkCreateGraphicsPipelines(device, cache, 1, &pc, NULL, &pipeline));
    size_t cache_size = 0; OK(vkGetPipelineCacheData(device, cache, &cache_size, NULL)); assert(cache_size >= 32);
    void *cache_data = malloc(cache_size); assert(cache_data); OK(vkGetPipelineCacheData(device, cache, &cache_size, cache_data));
    cache_info.initialDataSize = cache_size; cache_info.pInitialData = cache_data;
    OK(vkCreatePipelineCache(device, &cache_info, NULL, &cache2)); OK(vkMergePipelineCaches(device, cache2, 1, &cache));
    free(cache_data);
    vkDestroyPipeline(device, pipeline, NULL); vkDestroyPipelineCache(device, cache2, NULL); vkDestroyPipelineCache(device, cache, NULL);
    vkDestroyShaderModule(device, fragment, NULL); vkDestroyShaderModule(device, vertex, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL); vkDestroyRenderPass(device, pass, NULL); vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorUpdateTemplate(device, update, NULL); OK(vkFreeDescriptorSets(device, pool, 1, &set)); OK(vkResetDescriptorPool(device, pool, 0));
    vkDestroyDescriptorPool(device, pool, NULL); vkDestroyDescriptorSetLayout(device, layout, NULL);
    vkDestroySampler(device, sampler, NULL); vkDestroyImageView(device, view, NULL); vkDestroyImage(device, image, NULL); vkFreeMemory(device, im, NULL);
    vkDestroyBuffer(device, buffer, NULL); vkFreeMemory(device, bm, NULL);
    puts("guest Vulkan resources: memory round-trip, descriptors/templates, image, framebuffer, SPIR-V pipeline and cache passed");
}
