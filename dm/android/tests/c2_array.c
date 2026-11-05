// Execute every element of the real Maxwell driver-table shader arrays.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../gfx/nvn/vulkan/shader_contract.h"
#include "../../gfx/nvn/vulkan/driver_c2.h"
#include "c2_array_cases.h"
#define CHECK(call) do { VkResult r_=(call); if(r_) { fprintf(stderr,"%s: %d\n",#call,r_);return 1; } } while(0)
typedef struct { unsigned uniform,storage,images; int texture_off,sampler_off,sampler_or; } bindings_t;
static int load(VkDevice device,const char *directory,unsigned variant,VkShaderModule *module,bindings_t *bindings) {
    char path[1024],text[8192];
    int len=snprintf(path,sizeof path,"%s/export/00000000feed400%u.vk.txt",directory,variant);
    if(len<0 || (size_t)len>=sizeof path)return 1;
    FILE *file=fopen(path,"rb");if(!file)return 1;
    size_t size=fread(text,1,sizeof text-1,file);int ok=feof(file)&&!ferror(file);fclose(file);if(!ok)return 1;text[size]=0;
    snprintf(path,sizeof path,"%s/export/00000000feed400%u.spv",directory,variant);
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
        if(!strcmp(kind,"cbuf") && !strcmp(type,"ubo") && count==1 && reg==0) { bindings->uniform=binding;seen|=1; }
        else if(!strcmp(kind,"sbuf_ubo") && !strcmp(type,"ssbo") && count==1 && reg==0 && off==56) { bindings->storage=binding;seen|=2; }
        else if(!strcmp(kind,"tex_c2a") && !strcmp(type,"cis") && count==C2_TEST_COUNT && !strcmp(sampler,"samp_c2a")) {
            bindings->images=binding;bindings->texture_off=off;bindings->sampler_or=-1;
            const char *p=strstr(line," soff=");if(!p || sscanf(p," soff=%d",&bindings->sampler_off)!=1)continue;
            p=strstr(line," c2_or=");if(p && sscanf(p," c2_or=%d",&bindings->sampler_or)!=1)continue;
            seen|=4;
        }
    }
    if(seen!=7) { free(code);return 1; }
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
    VkResult result=vkCreateShaderModule(device,&ci,NULL,module);free(code);CHECK(result);return 0;
}
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *memory,uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memory->memoryTypeCount;i++) if((bits&(1u<<i)) && (memory->memoryTypes[i].propertyFlags&flags)==flags)return i;
    fprintf(stderr,"No compatible memory type\n");exit(1);
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;unsigned variant=(unsigned)strtoul(argv[2],NULL,10);if(variant>1)return 2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice physical;VkResult picked=vkEnumeratePhysicalDevices(instance,&count,&physical);
    if((picked!=VK_SUCCESS && picked!=VK_INCOMPLETE)||!count)return 1;
    VkPhysicalDeviceDescriptorIndexingFeatures indexing={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&indexing};vkGetPhysicalDeviceFeatures2(physical,&features);
    VkPhysicalDeviceDescriptorIndexingProperties limits={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&limits};vkGetPhysicalDeviceProperties2(physical,&props);
    if(!indexing.descriptorBindingSampledImageUpdateAfterBind || limits.maxPerStageDescriptorUpdateAfterBindSamplers<C2_TEST_COUNT ||
       limits.maxPerStageDescriptorUpdateAfterBindSampledImages<C2_TEST_COUNT) { fprintf(stderr,"Driver arrays require supported update-after-bind limits\n");return 77; }
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;if(family==count)return 77;
    float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice device;CHECK(vkCreateDevice(physical,&dci,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    bindings_t bindings={0};VkShaderModule shader;if(load(device,argv[1],variant,&shader,&bindings))return 1;
    VkBuffer buffers[2];VkDeviceMemory allocations[2];uint32_t *mapped[2];const unsigned sizes[]={65536,1024};
    for(unsigned i=0;i<2;i++) {
        VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizes[i],.usage=i?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        CHECK(vkCreateBuffer(device,&ci,NULL,&buffers[i]));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[i],&req);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device,&ai,NULL,&allocations[i]));CHECK(vkBindBufferMemory(device,buffers[i],allocations[i],0));CHECK(vkMapMemory(device,allocations[i],0,VK_WHOLE_SIZE,0,(void**)&mapped[i]));
    }
    VkImage images[3];VkDeviceMemory image_memory[3];VkImageView views[3];VkSampler samplers[4];
    for(unsigned i=0;i<3;i++) {
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R32_SFLOAT,.extent={1,1,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateImage(device,&ci,NULL,&images[i]));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,images[i],&req);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,0)};
        CHECK(vkAllocateMemory(device,&ai,NULL,&image_memory[i]));CHECK(vkBindImageMemory(device,images[i],image_memory[i],0));
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=images[i],.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32_SFLOAT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CHECK(vkCreateImageView(device,&vi,NULL,&views[i]));
    }
    for(unsigned i=0;i<4;i++) {
        VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.addressModeU=i>=2?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.borderColor=i==3?VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK};
        CHECK(vkCreateSampler(device,&ci,NULL,&samplers[i]));
    }
    VkDescriptorSetLayoutBinding lb[]={{bindings.uniform,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {bindings.storage,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{bindings.images,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,C2_TEST_COUNT,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorBindingFlags flags[]={0,0,VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,.bindingCount=3,.pBindingFlags=flags};
    VkDescriptorSetLayoutCreateInfo slci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.pNext=&bf,.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,.bindingCount=3,.pBindings=lb};
    VkDescriptorSetLayout set_layout;CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layout));
    VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,C2_TEST_COUNT}};
    VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.flags=VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,.maxSets=1,.poolSizeCount=3,.pPoolSizes=ps};
    VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&pool));
    VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
    uint64_t tx[256],im[32],sp[128];c2_test_inputs(tx,im,sp);VkDescriptorImageInfo image_info[C2_TEST_COUNT];
    uint64_t fixed=bindings.sampler_or>=0?dm_vk_c2_handle(tx,im,sp,(unsigned)bindings.sampler_or):0;
    for(unsigned i=0;i<C2_TEST_COUNT;i++) {
        uint64_t texture=dm_vk_c2_handle(tx,im,sp,(uint64_t)bindings.texture_off+8u*i)&0xfffff;
        uint64_t sampler=(dm_vk_c2_handle(tx,im,sp,(uint64_t)bindings.sampler_off+8u*i)|fixed)>>20;
        if(texture>2 || sampler>3)return 1;
        image_info[i]=(VkDescriptorImageInfo){samplers[sampler],views[texture],VK_IMAGE_LAYOUT_GENERAL};
    }
    VkDescriptorBufferInfo bi[]={{buffers[0],0,sizes[0]},{buffers[1],256,4}};
    VkWriteDescriptorSet writes[3];
    for(unsigned i=0;i<3;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=lb[i].binding,.descriptorCount=lb[i].descriptorCount,.descriptorType=lb[i].descriptorType,.pBufferInfo=i<2?&bi[i]:NULL,.pImageInfo=i==2?image_info:NULL};
    vkUpdateDescriptorSets(device,3,writes,0,NULL);
    VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
    VkComputePipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};
    VkPipeline pipeline;CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VkCommandPool commands;CHECK(vkCreateCommandPool(device,&cpci,NULL,&commands));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
    for(unsigned i=0;i<3;i++) {
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=images[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkClearColorValue value={.float32={i==1?0.25f:i==2?0.75f:0,0,0,1}};vkCmdClearColorImage(command,images[i],VK_IMAGE_LAYOUT_GENERAL,&value,1,&barrier.subresourceRange);
    }
    VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    unsigned cases=0;
    for(unsigned index=0;index<C2_TEST_INDICES;index++)for(unsigned coord=0;coord<C2_TEST_COORDS;coord++) {
        memset(mapped[0],0,sizes[0]);mapped[0][0]=c2_test_offset(index);mapped[0][1]=c2_test_coords[coord];memset(mapped[1],0xcc,sizes[1]);
        CHECK(vkResetCommandBuffer(command,0));CHECK(vkBeginCommandBuffer(command,&begin));vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(command,1,1,1);
        VkMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&done,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
        for(unsigned word=0;word<sizes[1]/4;word++) {
            uint32_t expected=word==64?c2_test_expected(index,coord,variant):0xcccccccc;
            if(mapped[1][word]!=expected) { fprintf(stderr,"C2 variant%u index%u coord%u word%u: %08x expected %08x\n",variant,index,coord,word,mapped[1][word],expected);return 1; }
        }
        if(fwrite(mapped[1]+64,4,1,stdout)!=1)return 1;cases++;
    }
    vkDestroyCommandPool(device,commands,NULL);vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyShaderModule(device,shader,NULL);vkDestroyDescriptorPool(device,pool,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    for(unsigned i=0;i<4;i++)vkDestroySampler(device,samplers[i],NULL);
    for(unsigned i=0;i<3;i++) { vkDestroyImageView(device,views[i],NULL);vkDestroyImage(device,images[i],NULL);vkFreeMemory(device,image_memory[i],NULL); }
    for(unsigned i=0;i<2;i++) { vkUnmapMemory(device,allocations[i]);vkDestroyBuffer(device,buffers[i],NULL);vkFreeMemory(device,allocations[i],NULL); }
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);fprintf(stderr,"Vulkan C2 variant%u: %u cases passed\n",variant,cases);return 0;
}
