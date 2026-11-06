// Execute actual Maxwell draw IDs through native and fallback draw parameters.
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include "../../gfx/nvn/vulkan/shader_contract.h"
#include "draw_ids_cases.h"
#include "../../gfx/nvn/vulkan/spv_draw_parameters.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CHECK(call) do { VkResult r_=(call); if(r_) { fprintf(stderr,"%s: %d\n",#call,r_);return 1; } } while(0)
typedef struct { unsigned storage,driver; } bindings_t;
static int load(VkDevice device,const char *directory,VkShaderModule *module,bindings_t *bindings,int fallback) {
    char path[1024],text[8192];
    int len=snprintf(path,sizeof path,"%s/export/%016llx.vk.txt",directory,0xfeed9200ull);
    if(len<0 || (size_t)len>=sizeof path)return 1;
    FILE *file=fopen(path,"rb");if(!file)return 1;
    size_t size=fread(text,1,sizeof text-1,file);int ok=feof(file)&&!ferror(file);fclose(file);if(!ok)return 1;text[size]=0;
    len=snprintf(path,sizeof path,"%s/export/%016llx.spv",directory,0xfeed9200ull);
    if(len<0 || (size_t)len>=sizeof path)return 1;
    file=fopen(path,"rb");if(!file)return 1;
    if(fseek(file,0,SEEK_END)) { fclose(file);return 1; }
    long bytes=ftell(file);if(bytes<20 || bytes%4 || fseek(file,0,SEEK_SET)) { fclose(file);return 1; }
    uint32_t *code=malloc((size_t)bytes);if(!code) { fclose(file);return 1; }
    ok=fread(code,1,(size_t)bytes,file)==(size_t)bytes;fclose(file);if(!ok) { free(code);return 1; }
    const char *error=dm_vk_shader_contract(text,size,code,(size_t)bytes/4);
    if(error) { fprintf(stderr,"%s\n",error);free(code);return 1; }
    unsigned seen=0;
    for(char *line=strtok(text,"\n");line;line=strtok(NULL,"\n")) {
        unsigned binding,count;int reg,off,sreg;char type[8],kind[16],sampler[16];
        if(sscanf(line,"b %u %7s %u %15s %d %d %15s %d",&binding,type,&count,kind,&reg,&off,sampler,&sreg)!=8)continue;
        if(!strcmp(kind,"sbuf_ubo")&&!strcmp(type,"ssbo")&&count==1&&reg==0&&off==56) { bindings->storage=binding;seen|=2; }
        else if(!strcmp(kind,"drvcb")&&!strcmp(type,"ubo")&&count==1&&off==0) { bindings->driver=binding;seen|=1; }
        else { free(code);return 1; }
    }
    if(seen!=3) { free(code);return 1; }
    if(fallback) {
        size_t count=0;int transformed=0;uint32_t *lowered=spv_draw_parameters(code,(size_t)bytes/4,0,&count,&transformed);
        free(code);if(!transformed||!lowered)return 1;code=lowered;bytes=(long)(count*4);
    }
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
    VkResult result=vkCreateShaderModule(device,&ci,NULL,module);free(code);CHECK(result);return 0;
}
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *memory,uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memory->memoryTypeCount;i++) if((bits&(1u<<i)) && (memory->memoryTypes[i].propertyFlags&flags)==flags)return i;
    fprintf(stderr,"No compatible memory type\n");exit(1);
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;int fallback=!strcmp(argv[2],"fallback");if(!fallback&&strcmp(argv[2],"native"))return 2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice physical;VkResult picked=vkEnumeratePhysicalDevices(instance,&count,&physical);
    if((picked!=VK_SUCCESS && picked!=VK_INCOMPLETE)||!count)return 1;
    VkPhysicalDeviceShaderDrawParametersFeatures drawFeatures={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceFeatures2 available={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&drawFeatures};vkGetPhysicalDeviceFeatures2(physical,&available);
    if(!available.features.vertexPipelineStoresAndAtomics||(!fallback&&!drawFeatures.shaderDrawParameters))return 77;
    VkPhysicalDeviceFeatures features={.vertexPipelineStoresAndAtomics=VK_TRUE};drawFeatures.shaderDrawParameters=!fallback;
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))family++;if(family==count)return 77;
    float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&drawFeatures,.pEnabledFeatures=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice device;CHECK(vkCreateDevice(physical,&dci,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    bindings_t bindings={0};VkShaderModule shader;int loaded=load(device,argv[1],&shader,&bindings,fallback);if(loaded)return loaded;
    VkBuffer buffers[2];VkDeviceMemory allocations[2];uint32_t *mapped[2];const unsigned sizes[]={4096,1024};
    for(unsigned i=0;i<2;i++) {
        VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizes[i],.usage=i==1?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        CHECK(vkCreateBuffer(device,&ci,NULL,&buffers[i]));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[i],&req);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device,&ai,NULL,&allocations[i]));CHECK(vkBindBufferMemory(device,buffers[i],allocations[i],0));CHECK(vkMapMemory(device,allocations[i],0,VK_WHOLE_SIZE,0,(void**)&mapped[i]));
    }
    unsigned binding_count=2;
    VkDescriptorSetLayoutBinding lb[]={{bindings.driver,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,NULL},
        {bindings.storage,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo slci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=binding_count,.pBindings=lb};
    VkDescriptorSetLayout set_layout;CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layout));
    VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};
    VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&pool));
    VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
    VkDescriptorBufferInfo bi[]={{buffers[0],0,4096},{buffers[1],256,16*4}};
    VkWriteDescriptorSet writes[2];
    for(unsigned i=0;i<binding_count;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=lb[i].binding,.descriptorCount=1,.descriptorType=lb[i].descriptorType,.pBufferInfo=&bi[i]};
    vkUpdateDescriptorSets(device,binding_count,writes,0,NULL);
    VkPushConstantRange push={VK_SHADER_STAGE_VERTEX_BIT,0,16};
    VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout,.pushConstantRangeCount=fallback?1:0,.pPushConstantRanges=fallback?&push:NULL};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS};
    VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.subpassCount=1,.pSubpasses=&subpass};VkRenderPass pass;CHECK(vkCreateRenderPass(device,&rpci,NULL,&pass));
    VkFramebufferCreateInfo fbci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.width=1,.height=1,.layers=1};VkFramebuffer framebuffer;CHECK(vkCreateFramebuffer(device,&fbci,NULL,&framebuffer));
    VkPipelineShaderStageCreateInfo stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=shader,.pName="main"};
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkViewport viewport={0,0,1,1,0,1};VkRect2D scissor={{0,0},{1,1}};
    VkPipelineViewportStateCreateInfo view={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.rasterizerDiscardEnable=VK_TRUE,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
    VkGraphicsPipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=1,.pStages=&stage,.pVertexInputState=&vertex,.pInputAssemblyState=&assembly,.pViewportState=&view,.pRasterizationState=&raster,.layout=layout,.renderPass=pass};
    VkPipeline pipeline;CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
    VkBuffer indexBuffer;VkDeviceMemory indexMemory;void *indexBytes;
    VkBufferCreateInfo ibci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4,.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT};CHECK(vkCreateBuffer(device,&ibci,NULL,&indexBuffer));
    VkMemoryRequirements indexReq;vkGetBufferMemoryRequirements(device,indexBuffer,&indexReq);
    VkMemoryAllocateInfo imai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=indexReq.size,.memoryTypeIndex=memory_type(&memory,indexReq.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(device,&imai,NULL,&indexMemory));CHECK(vkBindBufferMemory(device,indexBuffer,indexMemory,0));CHECK(vkMapMemory(device,indexMemory,0,VK_WHOLE_SIZE,0,&indexBytes));
    memcpy(indexBytes,(uint16_t[]){9,24},4);
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VkCommandPool commands;CHECK(vkCreateCommandPool(device,&cpci,NULL,&commands));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    for(unsigned test=0;test<DI_CASES;test++) {
        memset(mapped[0],0,4096);mapped[0][0x644/4]=di_base(test);((float*)mapped[0])[0x610/4]=2047;
        if(getenv("DI_WRONG_BASE"))mapped[0][0x644/4]++;
        memset(mapped[1],0xcc,sizes[1]);CHECK(vkResetCommandBuffer(command,0));CHECK(vkBeginCommandBuffer(command,&begin));
        VkRenderPassBeginInfo rpbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,.renderArea={{0,0},{1,1}}};
        vkCmdBeginRenderPass(command,&rpbi,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);
        if(fallback) { uint32_t words[]={(uint32_t)(di_indexed(test)?di_vertex_base(test):di_first(test)),di_base(test),0,0};vkCmdPushConstants(command,layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof words,words); }
        if(di_indexed(test)) { vkCmdBindIndexBuffer(command,indexBuffer,0,VK_INDEX_TYPE_UINT16);vkCmdDrawIndexed(command,1,di_instances(test),di_index_offset(test),di_vertex_base(test),di_base(test)); }
        else vkCmdDraw(command,1,di_instances(test),di_first(test),di_base(test));
        vkCmdEndRenderPass(command);
        VkMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&done,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
        if(di_check(test,mapped[1]))return 1;
    }
    vkUnmapMemory(device,indexMemory);vkDestroyBuffer(device,indexBuffer,NULL);vkFreeMemory(device,indexMemory,NULL);vkDestroyFramebuffer(device,framebuffer,NULL);vkDestroyRenderPass(device,pass,NULL);
    vkDestroyCommandPool(device,commands,NULL);vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyShaderModule(device,shader,NULL);vkDestroyDescriptorPool(device,pool,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    for(unsigned i=0;i<2;i++) { vkUnmapMemory(device,allocations[i]);vkDestroyBuffer(device,buffers[i],NULL);vkFreeMemory(device,allocations[i],NULL); }
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);fprintf(stderr,"Vulkan guest draw IDs:32 cases passed\n");return 0;
}
