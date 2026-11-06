// Return descriptors for each chunk with aligned offsets and within both the
// storage range and the dispatch size supported by this device. A descriptor
// stays bound to its allocation if later staging spills into a new ring.
static void packed_dispatch(VkDescriptorBufferInfo guest, VkDescriptorBufferInfo depth, VkDescriptorBufferInfo stencil,
                            size_t pixels, VkDeviceSize align, const vtex *v, int upload) {
    static const uint32_t code[] =
#include "packed_depth.comp.inc"
    ;
    static shader_t shader = {.key="packed-depth", .stage=5, .nb=3,
                              .b={{.type=BT_SSBO,.count=1},{.type=BT_SSBO,.count=1},{.type=BT_SSBO,.count=1}}};
    if (!shader.cpp) {
        clear_shader(&shader, code, sizeof code);
        VkDescriptorSetLayoutBinding bindings[3];
        for (unsigned i=0; i<3; ++i)
            bindings[i] = (VkDescriptorSetLayoutBinding){i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
        VkDescriptorSetLayoutCreateInfo set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,3,bindings};
        VKCHECK(vkCreateDescriptorSetLayout(g_dev,&set,NULL,&shader.dsl));
        VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT,0,5*sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,1,&shader.dsl,1,&push};
        VKCHECK(vkCreatePipelineLayout(g_dev,&layout,NULL,&shader.cpl));
        VkComputePipelineCreateInfo pipeline = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,NULL,0,
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_COMPUTE_BIT,shader.mod,"main",NULL},shader.cpl};
        VKCHECK(vkCreateComputePipelines(g_dev,g_pcache,1,&pipeline,NULL,&shader.cpp));
    }
    int guest_float = v->nvn_fmt == NVN_FORMAT_DEPTH32F_STENCIL8, has_stencil = v->nvn_fmt != NVN_FORMAT_DEPTH24;
    unsigned bpp = guest_float ? 8 : 4;
    size_t limit = g_props.limits.maxStorageBufferRange / bpp;
    size_t dispatch_limit = (size_t)g_props.limits.maxComputeWorkGroupCount[0] * 256;
    if (limit > dispatch_limit) limit = dispatch_limit;
    limit -= limit % align;   // align is at least 16, including the four-pixel stencil unit
    vkCmdBindPipeline(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,shader.cpp);
    for (size_t start=0; start<pixels;) {
        uint32_t count = (uint32_t)(pixels-start < limit ? pixels-start : limit);
        VkDescriptorBufferInfo buffers[] = {
            {guest.buffer,guest.offset+bpp*start,bpp*(VkDeviceSize)count},
            {depth.buffer,depth.offset+4*start,4*(VkDeviceSize)count},
            {stencil.buffer,stencil.offset+(has_stencil ? start : 0),has_stencil ? ((count+3u)&~3u) : 4}
        };
        VkDescriptorSet set = allocate_shader_set(&shader);
        VkWriteDescriptorSet writes[3];
        for (unsigned i=0; i<3; ++i)
            writes[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,set,i,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,NULL,&buffers[i],NULL};
        vkUpdateDescriptorSets(g_dev,3,writes,0,NULL);
        vkCmdBindDescriptorSets(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,shader.cpl,0,1,&set,0,NULL);
        uint32_t params[] = {count,upload,guest_float,v->f.f==VK_FORMAT_D32_SFLOAT_S8_UINT,has_stencil};
        vkCmdPushConstants(C->cb,shader.cpl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,params);
        vkCmdDispatch(C->cb,(count+255)/256,1,1);
        start += count;
    }
}

static void plane_transfer(vtex *v, int upload, VkBufferImageCopy region, VkDescriptorBufferInfo plane, VkImageAspectFlags aspect) {
    region.bufferOffset = plane.offset;
    region.imageSubresource.aspectMask = aspect;
    if (upload) vkCmdCopyBufferToImage(C->cb,plane.buffer,v->img,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    else vkCmdCopyImageToBuffer(C->cb,v->img,VK_IMAGE_LAYOUT_GENERAL,plane.buffer,1,&region);
}

static void buffer_image_transfer(vtex *v, VkBufferImageCopy region, VkDescriptorBufferInfo guest, int upload, VkDeviceSize align) {
    if (!v->f.stencil || v->nvn_fmt == NVN_FORMAT_STENCIL8) {
        plane_transfer(v,upload,region,guest,v->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : v->aspect);
    } else {
        size_t pixels=(size_t)region.imageExtent.width*region.imageExtent.height*region.imageExtent.depth*region.imageSubresource.layerCount;
        VkDeviceSize off=ring_alloc(4*pixels,align);
        VkDescriptorBufferInfo depth={C->ring,off,4*pixels}, stencil=depth;
        int has_stencil = v->nvn_fmt != NVN_FORMAT_DEPTH24;
        if (has_stencil) {
            VkDeviceSize bytes=(pixels+3)&~(size_t)3;
            off=ring_alloc(bytes,align);
            stencil=(VkDescriptorBufferInfo){C->ring,off,bytes};
        }
        if (upload) { packed_dispatch(guest,depth,stencil,pixels,align,v,1); barrier_all(C->cb); }
        plane_transfer(v,upload,region,depth,VK_IMAGE_ASPECT_DEPTH_BIT);
        if (has_stencil) plane_transfer(v,upload,region,stencil,VK_IMAGE_ASPECT_STENCIL_BIT);
        if (!upload) { barrier_all(C->cb); packed_dispatch(guest,depth,stencil,pixels,align,v,0); }
    }
    barrier_all(C->cb);
}
