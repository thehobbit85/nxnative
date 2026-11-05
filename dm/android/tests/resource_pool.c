// Exercise bounded runtime pools with different handles in the same workgroup.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../gfx/nvn/vulkan/shader_contract.h"
#include "resource_pool_cases.h"
#define CHECK(call) do { VkResult r_=(call);if(r_) { fprintf(stderr,"%s: %d\n",#call,r_);return 1; } } while(0)
enum { RP_UNIFORM,RP_INPUT,RP_OUTPUT,RP_BOUNDS,RP_TEXTURES,RP_SAMPLERS,RP_BINDINGS };
typedef struct { unsigned number,slot;VkDescriptorType type; } binding_t;
static int load(VkDevice device,const char *directory,VkShaderModule *module,binding_t *bindings) {
    char path[1024],text[8192];int len=snprintf(path,sizeof path,"%s/export/00000000feed6000.vk.txt",directory);
    if(len<0 || (size_t)len>=sizeof path)return 1;
    FILE *file=fopen(path,"rb");if(!file)return 1;size_t size=fread(text,1,sizeof text-1,file);int ok=feof(file)&&!ferror(file);fclose(file);if(!ok)return 1;text[size]=0;
    snprintf(path,sizeof path,"%s/export/00000000feed6000.spv",directory);file=fopen(path,"rb");if(!file)return 1;
    if(fseek(file,0,SEEK_END)) { fclose(file);return 1; }long bytes=ftell(file);if(bytes<20 || bytes%4 || fseek(file,0,SEEK_SET)) { fclose(file);return 1; }
    uint32_t *code=malloc((size_t)bytes);if(!code) { fclose(file);return 1; }ok=fread(code,1,(size_t)bytes,file)==(size_t)bytes;fclose(file);if(!ok) { free(code);return 1; }
    const char *error=dm_vk_shader_contract(text,size,code,(size_t)bytes/4);if(error) { fprintf(stderr,"%s\n",error);free(code);return 1; }
    unsigned seen=0;
    for(char *line=strtok(text,"\n");line;line=strtok(NULL,"\n")) {
        unsigned number,count,slot=0,index=RP_BINDINGS;int reg,off,sreg;char type[8],kind[16],sampler[16];VkDescriptorType descriptor=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if(sscanf(line,"b %u %7s %u %15s %d %d %15s %d",&number,type,&count,kind,&reg,&off,sampler,&sreg)!=8)continue;
        if(!strcmp(kind,"cbuf") && !strcmp(type,"ubo") && count==1 && reg==0) { index=RP_UNIFORM;descriptor=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; }
        else if(!strcmp(kind,"sbuf_ubo") && !strcmp(type,"ssbo") && count==1 && reg==0 && (off==0 || off==56))index=off?RP_OUTPUT:RP_INPUT;
        else if(!strcmp(kind,"pool_bounds") && !strcmp(type,"ubo") && count==1 && reg==2) { index=RP_BOUNDS;descriptor=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; }
        else if(!strcmp(kind,"tex_pool") && !strcmp(type,"tex") && count==1048576) { index=RP_TEXTURES;descriptor=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; }
        else if(!strcmp(kind,"samp_pool") && !strcmp(type,"samp") && count==4096) { index=RP_SAMPLERS;descriptor=VK_DESCRIPTOR_TYPE_SAMPLER; }
        if(index>=RP_TEXTURES && index<RP_BINDINGS) {
            const char *field=strstr(line," runtime=1 pool_slot=");if(!field || sscanf(field," runtime=1 pool_slot=%u",&slot)!=1 || slot>=2)index=RP_BINDINGS;
        }
        if(index==RP_BINDINGS || (seen&(1u<<index))) { free(code);return 1; }seen|=1u<<index;bindings[index]=(binding_t){number,slot,descriptor};
    }
    if(seen!=63 || bindings[RP_TEXTURES].slot==bindings[RP_SAMPLERS].slot) { free(code);return 1; }
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};VkResult result=vkCreateShaderModule(device,&ci,NULL,module);free(code);CHECK(result);return 0;
}
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *memory,uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memory->memoryTypeCount;i++)if((bits&(1u<<i)) && (memory->memoryTypes[i].propertyFlags&flags)==flags)return i;
    fprintf(stderr,"No compatible memory type\n");exit(1);
}
int main(int argc,char **argv) {
    if(argc<2 || argc>3)return 2;int negative=argc==3 && !strcmp(argv[2],"short-bound");if(argc==3 && !negative)return 2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice physical;VkResult picked=vkEnumeratePhysicalDevices(instance,&count,&physical);if((picked!=VK_SUCCESS && picked!=VK_INCOMPLETE)||!count)return 1;
    VkPhysicalDeviceDescriptorIndexingFeatures indexing={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&indexing};vkGetPhysicalDeviceFeatures2(physical,&features);
    VkPhysicalDeviceDescriptorIndexingProperties limits={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&limits};vkGetPhysicalDeviceProperties2(physical,&properties);
    unsigned max_textures=rp_texture_bounds[RP_CONFIGS-1]+1,max_samplers=rp_sampler_bounds[RP_CONFIGS-1]+1;
    if(!indexing.runtimeDescriptorArray || !indexing.shaderSampledImageArrayNonUniformIndexing || !indexing.descriptorBindingSampledImageUpdateAfterBind ||
       limits.maxPerStageDescriptorUpdateAfterBindSampledImages<max_textures || limits.maxPerStageDescriptorUpdateAfterBindSamplers<max_samplers ||
       limits.maxDescriptorSetUpdateAfterBindSampledImages<max_textures || limits.maxDescriptorSetUpdateAfterBindSamplers<max_samplers ||
       limits.maxPerStageUpdateAfterBindResources<max_textures+max_samplers+4 || limits.maxUpdateAfterBindDescriptorsInAllPools<max_textures+max_samplers+4) {
        fprintf(stderr,"Runtime pool fixture requires supported descriptor-indexing features and limits\n");return 77;
    }
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;if(family==count)return 77;
    float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};VkDevice device;CHECK(vkCreateDevice(physical,&dci,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);binding_t bindings[RP_BINDINGS];VkShaderModule shader;if(load(device,argv[1],&shader,bindings))return 1;
    VkBuffer buffers[4];VkDeviceMemory allocations[4];uint32_t *mapped[4];const unsigned sizes[]={65536,1024,1024,256};
    for(unsigned i=0;i<4;i++) {
        VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizes[i],.usage=i==RP_UNIFORM || i==RP_BOUNDS?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};CHECK(vkCreateBuffer(device,&ci,NULL,&buffers[i]));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[i],&req);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};CHECK(vkAllocateMemory(device,&ai,NULL,&allocations[i]));CHECK(vkBindBufferMemory(device,buffers[i],allocations[i],0));CHECK(vkMapMemory(device,allocations[i],0,VK_WHOLE_SIZE,0,(void**)&mapped[i]));
    }
    VkImage images[3];VkDeviceMemory image_memory[3];VkImageView views[3];VkSampler samplers[4];
    for(unsigned i=0;i<3;i++) {
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R32_SFLOAT,.extent={1,1,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};CHECK(vkCreateImage(device,&ci,NULL,&images[i]));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,images[i],&req);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,0)};CHECK(vkAllocateMemory(device,&ai,NULL,&image_memory[i]));CHECK(vkBindImageMemory(device,images[i],image_memory[i],0));VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=images[i],.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32_SFLOAT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};CHECK(vkCreateImageView(device,&vi,NULL,&views[i]));
    }
    for(unsigned i=0;i<4;i++) { VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.addressModeU=i>=2?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.borderColor=i==3?VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK};CHECK(vkCreateSampler(device,&ci,NULL,&samplers[i])); }
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VkCommandPool commands;CHECK(vkCreateCommandPool(device,&cpci,NULL,&commands));VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
    for(unsigned i=0;i<3;i++) {
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=images[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkClearColorValue value={.float32={i==1?0.25f:i==2?0.75f:0,0,0,1}};vkCmdClearColorImage(command,images[i],VK_IMAGE_LAYOUT_GENERAL,&value,1,&barrier.subresourceRange);
    }
    VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    unsigned cases=0;
    for(unsigned config=0;config<RP_CONFIGS;config++) {
        unsigned nt=rp_texture_bounds[config],ns=rp_sampler_bounds[config];VkDescriptorSetLayoutBinding lb[RP_BINDINGS];VkDescriptorBindingFlags flags[RP_BINDINGS];
        for(unsigned i=0;i<RP_BINDINGS;i++) { lb[i]=(VkDescriptorSetLayoutBinding){bindings[i].number,bindings[i].type,i==RP_TEXTURES?nt+1:i==RP_SAMPLERS?ns+1:1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};flags[i]=i>=RP_TEXTURES?VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT:0; }
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,.bindingCount=RP_BINDINGS,.pBindingFlags=flags};VkDescriptorSetLayoutCreateInfo slci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.pNext=&bf,.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,.bindingCount=RP_BINDINGS,.pBindings=lb};VkDescriptorSetLayout set_layout;CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layout));
        VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,nt+1},{VK_DESCRIPTOR_TYPE_SAMPLER,ns+1}};VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.flags=VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,.maxSets=1,.poolSizeCount=4,.pPoolSizes=ps};VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&pool));VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
        VkDescriptorImageInfo *textures=calloc(nt+1,sizeof *textures),*sampler_info=calloc(ns+1,sizeof *sampler_info);if(!textures || !sampler_info)return 1;
        for(unsigned i=0;i<=nt;i++)textures[i]=(VkDescriptorImageInfo){VK_NULL_HANDLE,views[rp_texture(i,nt)],VK_IMAGE_LAYOUT_GENERAL};for(unsigned i=0;i<=ns;i++)sampler_info[i]=(VkDescriptorImageInfo){samplers[rp_sampler(i,ns)],VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
        VkDescriptorBufferInfo bi[]={{buffers[0],0,sizes[0]},{buffers[1],256,32},{buffers[2],256,32},{buffers[3],0,32}};VkWriteDescriptorSet writes[RP_BINDINGS];
        for(unsigned i=0;i<RP_BINDINGS;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=lb[i].binding,.descriptorCount=lb[i].descriptorCount,.descriptorType=lb[i].descriptorType,.pBufferInfo=i<4?&bi[i]:NULL,.pImageInfo=i==RP_TEXTURES?textures:i==RP_SAMPLERS?sampler_info:NULL};vkUpdateDescriptorSets(device,RP_BINDINGS,writes,0,NULL);free(textures);free(sampler_info);
        VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));VkComputePipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};VkPipeline pipeline;CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
        for(unsigned test=0;test<RP_INDICES;test++)for(unsigned coord=0;coord<RP_COORDS;coord++) {
            memset(mapped[0],0,sizes[0]);mapped[0][0]=0x10000;mapped[0][14]=0x20000;mapped[0][4]=rp_coords[coord];memset(mapped[1],0xcc,sizes[1]);memset(mapped[2],0xcc,sizes[2]);memset(mapped[3],0,sizes[3]);
            for(unsigned lane=0;lane<RP_LANES;lane++)mapped[1][64+lane]=rp_handle(config,test,lane);mapped[3][4*bindings[RP_TEXTURES].slot]=negative?1:nt;mapped[3][4*bindings[RP_SAMPLERS].slot]=ns;
            CHECK(vkResetCommandBuffer(command,0));CHECK(vkBeginCommandBuffer(command,&begin));vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(command,1,1,1);VkMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&done,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
            for(unsigned word=0;word<sizes[2]/4;word++) { uint32_t expected=word>=64 && word<72?rp_expected(config,test,coord,word-64):0xcccccccc;if(mapped[2][word]!=expected) { fprintf(stderr,"Pool config%u index%u coord%u word%u: %08x expected %08x\n",config,test,coord,word,mapped[2][word],expected);return 1; } }
            if(fwrite(mapped[2]+64,4,RP_LANES,stdout)!=RP_LANES)return 1;cases+=RP_LANES;
        }
        vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyDescriptorPool(device,pool,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    }
    vkDestroyCommandPool(device,commands,NULL);vkDestroyShaderModule(device,shader,NULL);for(unsigned i=0;i<4;i++)vkDestroySampler(device,samplers[i],NULL);
    for(unsigned i=0;i<3;i++) { vkDestroyImageView(device,views[i],NULL);vkDestroyImage(device,images[i],NULL);vkFreeMemory(device,image_memory[i],NULL); }
    for(unsigned i=0;i<4;i++) { vkUnmapMemory(device,allocations[i]);vkDestroyBuffer(device,buffers[i],NULL);vkFreeMemory(device,allocations[i],NULL); }vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);fprintf(stderr,"Vulkan resource pools: %u samples passed\n",cases);return 0;
}
