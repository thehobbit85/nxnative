// Submit real guest commands, then compare GPU output with exact CPU oracles.
static __attribute__((noinline)) void command_checks(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, uint32_t family, VkQueue queue,
        VkBuffer source, VkDeviceMemory source_memory, VkImage image, VkFramebuffer framebuffer, VkRenderPass pass,
        VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet descriptors, VkPhysicalDeviceMemoryProperties *memory) {
#ifdef TEST_NATIVE_CONTROL
    extern PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice, const char *);
    gdpa = vkGetDeviceProcAddr;
#endif
    LOAD(vkCreateCommandPool); LOAD(vkDestroyCommandPool); LOAD(vkResetCommandPool); LOAD(vkTrimCommandPool);
    LOAD(vkAllocateCommandBuffers); LOAD(vkFreeCommandBuffers); LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer); LOAD(vkResetCommandBuffer);
    LOAD(vkCmdPipelineBarrier); LOAD(vkCmdBeginRenderPass); LOAD(vkCmdEndRenderPass); LOAD(vkCmdBindPipeline); LOAD(vkCmdBindDescriptorSets); LOAD(vkCmdDraw);
    LOAD(vkCmdCopyImageToBuffer); LOAD(vkCmdCopyBuffer); LOAD(vkQueueSubmit); LOAD(vkQueueWaitIdle); LOAD(vkDeviceWaitIdle);
    LOAD(vkCreateFence); LOAD(vkDestroyFence); LOAD(vkWaitForFences); LOAD(vkResetFences); LOAD(vkGetFenceStatus);
    LOAD(vkCreateSemaphore); LOAD(vkDestroySemaphore); LOAD(vkCreateQueryPool); LOAD(vkDestroyQueryPool);
    LOAD(vkCmdResetQueryPool); LOAD(vkCmdWriteTimestamp); LOAD(vkGetQueryPoolResults);
    LOAD(vkCreateBuffer); LOAD(vkDestroyBuffer); LOAD(vkGetBufferMemoryRequirements); LOAD(vkBindBufferMemory);
    LOAD(vkAllocateMemory); LOAD(vkFreeMemory); LOAD(vkMapMemory); LOAD(vkUnmapMemory); LOAD(vkFlushMappedMemoryRanges); LOAD(vkInvalidateMappedMemoryRanges);

    uint32_t *mapped; OK(vkMapMemory(device, source_memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    float red[4] = {1, 0, 0, 1}; memcpy(mapped, red, sizeof red);
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = source_memory, .size = VK_WHOLE_SIZE};
    OK(vkFlushMappedMemoryRanges(device, 1, &range)); vkUnmapMemory(device, source_memory);
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 8192, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer readback; OK(vkCreateBuffer(device, &bc, NULL, &readback));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, readback, &mr);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = memory_type(memory, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    VkDeviceMemory destination; OK(vkAllocateMemory(device, &ma, NULL, &destination)); OK(vkBindBufferMemory(device, readback, destination, 0));
    VkCommandPoolCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool; OK(vkCreateCommandPool(device, &cp, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    VkCommandBuffer cb[2]; OK(vkAllocateCommandBuffers(device, &ca, cb));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkQueryPoolCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VkQueryPool queries; OK(vkCreateQueryPool(device, &qc, NULL, &queries));
    OK(vkBeginCommandBuffer(cb[0], &begin));
    vkCmdResetQueryPool(cb[0], queries, 0, 2); vkCmdWriteTimestamp(cb[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT};
    // Ten integer arguments: the last two live on the guest stack. This must not
    // use the register-only entry thunk; the image barrier below also reads them.
    vkCmdPipelineBarrier(cb[0], VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    VkClearValue blue = {.color.float32 = {0, 0, 1, 1}};
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass, .framebuffer = framebuffer,
        .renderArea = {{0, 0}, {16, 16}}, .clearValueCount = 1, .pClearValues = &blue};
    vkCmdBeginRenderPass(cb[0], &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb[0], VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptors, 0, NULL);
    if (!getenv("TEST_CLEAR_ONLY")) vkCmdDraw(cb[0], 3, 1, 0, 0);
    vkCmdEndRenderPass(cb[0]);
    VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb[0], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    vkCmdWriteTimestamp(cb[0], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1); OK(vkEndCommandBuffer(cb[0]));
    OK(vkBeginCommandBuffer(cb[1], &begin));
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {16, 16, 1}};
    vkCmdCopyImageToBuffer(cb[1], image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    VkBufferCopy bytes = {0, 4096, 4096}; vkCmdCopyBuffer(cb[1], source, readback, 1, &bytes);
    VkMemoryBarrier cpu = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &cpu, 0, NULL, 0, NULL);
    OK(vkEndCommandBuffer(cb[1]));
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; OK(vkCreateFence(device, &fc, NULL, &fence));
    assert(vkGetFenceStatus(device, fence) == VK_NOT_READY);
    assert(vkWaitForFences(device, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT);
    VkSemaphoreCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore; OK(vkCreateSemaphore(device, &sc, NULL, &semaphore));
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit[2] = {{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb[0], .signalSemaphoreCount = 1, .pSignalSemaphores = &semaphore},
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore, .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cb[1]}};
    OK(vkQueueSubmit(queue, 2, submit, fence)); OK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    OK(vkGetFenceStatus(device, fence));
    uint64_t stamps[2]; OK(vkGetQueryPoolResults(device, queries, 0, 2, sizeof stamps, stamps, sizeof stamps[0], VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    assert(stamps[1] >= stamps[0] && stamps[1]);
    OK(vkMapMemory(device, destination, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    range.memory = destination; OK(vkInvalidateMappedMemoryRanges(device, 1, &range));
    uint32_t expected = getenv("TEST_CLEAR_ONLY") ? 0xffff0000u : 0xff0000ffu;
    for (unsigned i = 0; i < 256; ++i) if (mapped[i] != expected) {
        fprintf(stderr, "pixel %u: got %08x, expected %08x; uniform copy %08x %08x %08x %08x\n",
                i, mapped[i], expected, mapped[1024], mapped[1025], mapped[1026], mapped[1027]);
        assert(!"rendered pixel mismatch");
    }
    assert(!memcmp(mapped + 1024, red, sizeof red));
    for (unsigned i = 4; i < 1024; ++i) assert(mapped[1024 + i] == (i ^ 0x12345678u));
    vkUnmapMemory(device, destination);
    OK(vkResetFences(device, 1, &fence)); assert(vkGetFenceStatus(device, fence) == VK_NOT_READY);
    OK(vkResetCommandBuffer(cb[0], 0)); OK(vkBeginCommandBuffer(cb[0], &begin)); OK(vkEndCommandBuffer(cb[0]));
    submit[0].signalSemaphoreCount = 0; OK(vkQueueSubmit(queue, 1, submit, fence));
    OK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX)); OK(vkQueueWaitIdle(queue)); OK(vkDeviceWaitIdle(device));
    OK(vkResetCommandPool(device, pool, 0)); vkTrimCommandPool(device, pool, 0);
    vkFreeCommandBuffers(device, pool, 2, cb); vkDestroyCommandPool(device, pool, NULL);
    vkDestroyQueryPool(device, queries, NULL); vkDestroySemaphore(device, semaphore, NULL); vkDestroyFence(device, fence, NULL);
    vkDestroyBuffer(device, readback, NULL); vkFreeMemory(device, destination, NULL);
    puts("guest Vulkan execution: 256 exact pixels, GPU buffer copy, 10-argument barriers, semaphore/fence/timestamps and reset passed");
}
