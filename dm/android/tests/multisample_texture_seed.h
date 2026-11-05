// Populate every sample with independent per-pixel/per-layer values on the GPU.
#ifndef MULTISAMPLE_TEXTURE_SEED_H
#define MULTISAMPLE_TEXTURE_SEED_H
static int ms_module(VkDevice device,const char *directory,const char *name,VkShaderModule *shader) {
    char path[1024];int len=snprintf(path,sizeof path,"%s/%s",directory,name);if(len<0 || (size_t)len>=sizeof path)return 1;
    FILE *file=fopen(path,"rb");if(!file)return 1;uint32_t words[4096];size_t size=fread(words,1,sizeof words,file);int ok=feof(file)&&!ferror(file);fclose(file);if(!ok || size<20 || size%4)return 1;
    VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=words};CHECK(vkCreateShaderModule(device,&info,NULL,shader));return 0;
}
static int ms_seed(VkDevice device,VkQueue queue,uint32_t family,VkImage image,unsigned layers,const char *directory) {
    VkShaderModule vertex,fragment;if(ms_module(device,directory,"fullscreen.spv",&vertex) || ms_module(device,directory,"seed.spv",&fragment))return 1;
    VkAttachmentDescription attachment={.format=VK_FORMAT_R32G32B32A32_SFLOAT,.samples=VK_SAMPLE_COUNT_4_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color};
    VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass};VkRenderPass pass;CHECK(vkCreateRenderPass(device,&rpci,NULL,&pass));
    VkPushConstantRange push={VK_SHADER_STAGE_FRAGMENT_BIT,0,4};VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.pushConstantRangeCount=1,.pPushConstantRanges=&push};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
    VkPipelineShaderStageCreateInfo stages[]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,MS_SIDE,MS_SIDE,0,1};VkRect2D scissor={{0,0},{MS_SIDE,MS_SIDE}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo multi={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_4_BIT,.sampleShadingEnable=VK_TRUE,.minSampleShading=1};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
    VkGraphicsPipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&multi,.pColorBlendState=&cb,.layout=layout,.renderPass=pass};VkPipeline pipeline;CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};VkCommandPool pool;CHECK(vkCreateCommandPool(device,&cpci,NULL,&pool));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
    VkImageView views[3];VkFramebuffer frames[3];if(layers>3)return 1;
    for(uint32_t layer=0;layer<layers;layer++) {
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,layer,1}};CHECK(vkCreateImageView(device,&view,NULL,&views[layer]));
        VkFramebufferCreateInfo frame={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=1,.pAttachments=&views[layer],.width=MS_SIDE,.height=MS_SIDE,.layers=1};CHECK(vkCreateFramebuffer(device,&frame,NULL,&frames[layer]));
        VkClearValue clear={.color={{-1,-1,-1,-1}}};VkRenderPassBeginInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=frames[layer],.renderArea={{0,0},{MS_SIDE,MS_SIDE}},.clearValueCount=1,.pClearValues=&clear};
        vkCmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdPushConstants(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,4,&layer);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    }
    VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    for(unsigned i=0;i<layers;i++) { vkDestroyFramebuffer(device,frames[i],NULL);vkDestroyImageView(device,views[i],NULL); }
    vkDestroyCommandPool(device,pool,NULL);vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyRenderPass(device,pass,NULL);vkDestroyShaderModule(device,vertex,NULL);vkDestroyShaderModule(device,fragment,NULL);return 0;
}
#endif
