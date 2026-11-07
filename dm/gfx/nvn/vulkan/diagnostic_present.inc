// Use the present shader for output-sized screenshots: sampling and crop-edge
// clamping must be the same as the image displayed in the Android window.
static int diagnostic_render_locked(vtex *source,const int crop[4],uint32_t width,uint32_t height,
                                    int fit,diagnostic_image *out) {
    if (!width || !height || width>g_props.limits.maxImageDimension2D || height>g_props.limits.maxImageDimension2D ||
        (uint64_t)width*height*4>INT_MAX || source->samples!=VK_SAMPLE_COUNT_1_BIT ||
        source->aspect!=VK_IMAGE_ASPECT_COLOR_BIT || clear_color_type(source->f.f)!=CLEAR_FLOAT ||
        source->target==NVN_TEXTURE_TARGET_3D) {
        LOG("[RED] vk: screenshot output or source configuration is unsupported");return 0;
    }
    copy_draw_params region={.src={0,0,source->w,source->h},.dst={0,0,width,height},.linear=2};
    if (crop && (crop[2] || crop[3])) {
        if (crop[0]<0 || crop[1]<0 || crop[2]<=0 || crop[3]<=0 ||
            (uint64_t)crop[0]+crop[2]>source->w || (uint64_t)crop[1]+crop[3]>source->h) {
            LOG("[RED] vk: screenshot crop is outside the image");return 0;
        }
        memcpy(region.src,crop,sizeof region.src);
    }
    if (g_origin_ll) { region.src[1]+=region.src[3];region.src[3]=-region.src[3]; }
    if (fit) {
        const char *setting=getenv("DM_SCREEN_FIT");int stretch=setting && !strcmp(setting,"stretch");
        vk_present_rect rect=vk_present_fit(atomic_load(&g_screen_shape),atomic_load(&g_layer_shape),
            source->w,source->h,width,height,stretch);
        region.dst[0]=rect.x;region.dst[1]=rect.y;region.dst[2]=rect.w;region.dst[3]=rect.h;
    }
    VkImageView input=window_source(source);if (!input) return 0;
    static const uint32_t vertex_code[] =
#include "clear.vert.inc"
    ;
    static const uint32_t fragment_code[] =
#include "blit_color.frag.inc"
    ;
    static shader_t vertex={.key="screenshot-vertex"};
    static shader_t fragment={.key="screenshot-color",.stage=1,.nb=1,.b={{.type=BT_TEX,.count=1}}};
    static VkPipelineLayout layout;static VkPipeline pipeline;static VkDescriptorPool descriptors;static VkDescriptorSet set;
    // g_rmu serializes this diagnostic state and retains the source image.
    if (!layout) {
        clear_shader(&vertex,vertex_code,sizeof vertex_code);clear_shader(&fragment,fragment_code,sizeof fragment_code);
        layout=image_draw_layout(&fragment,sizeof region);
        VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1};
        VkDescriptorPoolCreateInfo pool={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,NULL,0,1,1,&size};
        VKCHECK(vkCreateDescriptorPool(g_dev,&pool,NULL,&descriptors));
        VkDescriptorSetAllocateInfo allocation={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,NULL,descriptors,1,&fragment.dsl};
        VKCHECK(vkAllocateDescriptorSets(g_dev,&allocation,&set));
    }
    if (!pipeline) {
        pso_key key={.vs=&vertex,.fs=&fragment,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.nc=1,.cf={VK_FORMAT_R8G8B8A8_UNORM},.chmask.mask=15};
        pipeline=pso_create(&key,layout);if (!pipeline) return 0;
    }
    VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;
    VkImageCreateInfo create={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    int ok=0;
    if (vkCreateImage(g_dev,&create,NULL,&image)!=VK_SUCCESS) goto cleanup;
    VkMemoryRequirements requirements;vkGetImageMemoryRequirements(g_dev,image,&requirements);
    VkMemoryAllocateInfo allocation={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,requirements.size,
        mem_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(g_dev,&allocation,NULL,&memory)!=VK_SUCCESS || vkBindImageMemory(g_dev,image,memory,0)!=VK_SUCCESS) goto cleanup;
    VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    if (vkCreateImageView(g_dev,&view_info,NULL,&view)!=VK_SUCCESS) goto cleanup;
    VkDescriptorImageInfo sampled={VK_NULL_HANDLE,input,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,set,0,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,&sampled};
    vkUpdateDescriptorSets(g_dev,1,&write,0,NULL);
    VkCommandBuffer command=once_begin();barrier_all(command);to_general(command,image,VK_IMAGE_ASPECT_COLOR_BIT);
    rp_desc pass={.n=1,.cv={view},.cf={VK_FORMAT_R8G8B8A8_UNORM},.cl={VK_ATTACHMENT_LOAD_OP_CLEAR},
        .cc[0].float32={0,0,0,1},.w=width,.h=height,.layout=VK_IMAGE_LAYOUT_GENERAL};
    VkFramebuffer framebuffer=rp_begin(command,&pass);
    VkRect2D rect={{region.dst[0],region.dst[1]},{region.dst[2],region.dst[3]}};
    internal_draw_state(command,pipeline,rect,(VkExtent2D){width,height},0);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);
    vkCmdPushConstants(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof region,&region);
    vkCmdDraw(command,3,1,0,0);rp_end(command);barrier_all(command);once_end(command);
    if (framebuffer) vkDestroyFramebuffer(g_dev,framebuffer,NULL);
    vtex converted={.img=image,.w=width,.h=height,.d=1,.levels=1,.layers=1,.target=NVN_TEXTURE_TARGET_2D,
        .f={.f=VK_FORMAT_R8G8B8A8_UNORM},.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.samples=VK_SAMPLE_COUNT_1_BIT};
    ok=diagnostic_read_locked(&converted,0,0,1,converted.f.f,out);
cleanup:
    if (view) vkDestroyImageView(g_dev,view,NULL);
    if (image) vkDestroyImage(g_dev,image,NULL);
    if (memory) vkFreeMemory(g_dev,memory,NULL);
    if (!ok) LOG("[RED] vk: screenshot output allocation or readback failed");
    return ok;
}
