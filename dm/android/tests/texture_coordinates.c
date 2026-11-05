// Run real Maxwell texture operations through the exported Vulkan contract.
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include "../../gfx/nvn/vulkan/shader_contract.h"
#include "texture_coordinates_cases.h"
#define CHECK(call) do { VkResult r_=(call); if(r_) { fprintf(stderr,"%s: %d\n",#call,r_);return 1; } } while(0)
typedef struct { unsigned uniform,storage,image,support; int has_support,extended_gather; } bindings_t;
static int load(VkDevice device,const char *directory,unsigned variant,VkShaderModule *module,bindings_t *bindings,int extended_gather) {
    char path[1024],text[8192];
    int len=snprintf(path,sizeof path,"%s/export/%016llx.vk.txt",directory,0xfeed9500ull+variant);
    if(len<0 || (size_t)len>=sizeof path)return 1;
    FILE *file=fopen(path,"rb");if(!file)return 1;
    size_t size=fread(text,1,sizeof text-1,file);int ok=feof(file)&&!ferror(file);fclose(file);if(!ok)return 1;text[size]=0;
    len=snprintf(path,sizeof path,"%s/export/%016llx.spv",directory,0xfeed9500ull+variant);
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
        if(!strcmp(line,"cap 25"))bindings->extended_gather=1;
        unsigned binding,count;int reg,off,sreg;char type[8],kind[16],sampler[16];
        if(sscanf(line,"b %u %7s %u %15s %d %d %15s %d",&binding,type,&count,kind,&reg,&off,sampler,&sreg)!=8)continue;
        if(!strcmp(kind,"cbuf") && !strcmp(type,"ubo") && count==1 && reg==0) { bindings->uniform=binding;seen|=1; }
        else if(!strcmp(kind,"sbuf_ubo") && !strcmp(type,"ssbo") && count==1 && reg==0 && off==56) { bindings->storage=binding;seen|=2; }
        else if(!strcmp(kind,"tex") && !strcmp(type,"cis") && count==1 && reg==128 && !strcmp(sampler,"samp") && sreg==1) { bindings->image=binding;seen|=4; }
        else if(!strcmp(kind,"support") && !strcmp(type,"ubo") && count==1) { bindings->support=binding;bindings->has_support=1; }
        else { free(code);return 1; }
    }
    if(seen!=7) { free(code);return 1; }
    if(bindings->extended_gather && !extended_gather) { fprintf(stderr,"Extended texture gather unsupported\n");free(code);return 77; }
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
    VkResult result=vkCreateShaderModule(device,&ci,NULL,module);free(code);CHECK(result);return 0;
}
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *memory,uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memory->memoryTypeCount;i++) if((bits&(1u<<i)) && (memory->memoryTypes[i].propertyFlags&flags)==flags)return i;
    fprintf(stderr,"No compatible memory type\n");exit(1);
}
int main(int argc,char **argv) {
    if(argc<3 || argc>4)return 2;unsigned variant=(unsigned)strtoul(argv[2],NULL,10),mode=argc==4?(unsigned)strtoul(argv[3],NULL,10):0;
    if(variant>3 || mode>1)return 2;unsigned cube=variant>=2,array=variant%2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice physical;VkResult picked=vkEnumeratePhysicalDevices(instance,&count,&physical);
    if((picked!=VK_SUCCESS && picked!=VK_INCOMPLETE)||!count)return 1;
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(physical,&features);
    VkFormatProperties format;vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32G32B32A32_SFLOAT,&format);
    if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))return 77;
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;if(family==count)return 77;
    float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    if(cube && array && !features.imageCubeArray)return 77;
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pEnabledFeatures=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice device;CHECK(vkCreateDevice(physical,&dci,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    bindings_t bindings={0};VkShaderModule shader;int loaded=load(device,argv[1],variant,&shader,&bindings,features.shaderImageGatherExtended);if(loaded)return loaded;
    VkBuffer buffers[4];VkDeviceMemory allocations[4];uint32_t *mapped[4];const unsigned sizes[]={65536,1024,4096,32768};
    for(unsigned i=0;i<4;i++) {
        VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizes[i],.usage=i==1?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:i==3?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        CHECK(vkCreateBuffer(device,&ci,NULL,&buffers[i]));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[i],&req);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device,&ai,NULL,&allocations[i]));CHECK(vkBindBufferMemory(device,buffers[i],allocations[i],0));CHECK(vkMapMemory(device,allocations[i],0,VK_WHOLE_SIZE,0,(void**)&mapped[i]));
    }
    memset(mapped[2],0,sizes[2]);for(unsigned i=0;i<73*4;i++)mapped[2][48+i]=tco_bits(1);
    VkImage image;VkDeviceMemory image_memory;VkImageView view;VkSampler samplers[2];
    VkImageCreateInfo imci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.extent={8,cube?8:1,1},.mipLevels=4,.arrayLayers=tco_layers(variant),.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(device,&imci,NULL,&image));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,image,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,0)};
    CHECK(vkAllocateMemory(device,&ai,NULL,&image_memory));CHECK(vkBindImageMemory(device,image,image_memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=cube?(array?VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:VK_IMAGE_VIEW_TYPE_CUBE):(array?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D),.format=VK_FORMAT_R32G32B32A32_SFLOAT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mode,4-mode,array?(cube?6:1):0,cube?(array?12:6):(array?3:1)}};
    CHECK(vkCreateImageView(device,&vi,NULL,&view));
    for(unsigned i=0;i<2;i++) {
        VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.minFilter=VK_FILTER_NEAREST,.magFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU=i?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=i?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=i?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.minLod=0,.maxLod=3};
        CHECK(vkCreateSampler(device,&ci,NULL,&samplers[i]));
    }
    unsigned binding_count=bindings.has_support?4:3;
    VkDescriptorSetLayoutBinding lb[]={{bindings.uniform,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {bindings.storage,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{bindings.image,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {bindings.support,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo slci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=binding_count,.pBindings=lb};
    VkDescriptorSetLayout set_layout;CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layout));
    VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};
    VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=3,.pPoolSizes=ps};
    VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&pool));
    VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
    VkDescriptorBufferInfo bi[]={{buffers[0],0,sizes[0]},{buffers[1],256,16},{buffers[2],0,sizes[2]}};
    VkDescriptorImageInfo image_info={samplers[0],view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[4];
    for(unsigned i=0;i<binding_count;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=lb[i].binding,.descriptorCount=1,.descriptorType=lb[i].descriptorType,.pBufferInfo=i==2?NULL:&bi[i==3?2:i],.pImageInfo=i==2?&image_info:NULL};
    vkUpdateDescriptorSets(device,binding_count,writes,0,NULL);
    VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
    VkComputePipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};
    VkPipeline pipeline;CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VkCommandPool commands;CHECK(vkCreateCommandPool(device,&cpci,NULL,&commands));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,4,0,tco_layers(variant)}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copies[4];unsigned offset=0;
    for(unsigned mip=0;mip<4;mip++) {
        unsigned size=tco_size(mip),height=cube?size:1,layers=tco_layers(variant);
        for(unsigned layer=0;layer<layers;layer++)tco_pixels((float*)((char*)mapped[3]+offset+layer*size*height*16),variant,mip,layer);
        copies[mip]=(VkBufferImageCopy){.bufferOffset=offset,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,layers},.imageExtent={size,height,1}};
        offset+=size*height*layers*16;
    }
    if(offset>sizes[3])return 1;
    vkCmdCopyBufferToImage(command,buffers[3],image,VK_IMAGE_LAYOUT_GENERAL,4,copies);
    VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    for(unsigned index=0;index<tco_count(variant);index++) {
        uint32_t expected[4];tco_inputs(variant,mode,index,mapped[0],expected);if(getenv("TCO_WRONG_LOD"))mapped[0][4]=tco_bits(3);if(cube && getenv("TCO_WRONG_FACE"))mapped[0][array?1:0]^=0x80000000;memset(mapped[1],0xcc,sizes[1]);
        image_info.sampler=samplers[tco_wrap(index)];vkUpdateDescriptorSets(device,1,&writes[2],0,NULL);
        CHECK(vkResetCommandBuffer(command,0));CHECK(vkBeginCommandBuffer(command,&begin));vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(command,1,1,1);
        VkMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&done,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
        if(tco_check(variant,mode,index,mapped[1],expected))return 1;
    }
    vkDestroyCommandPool(device,commands,NULL);vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyShaderModule(device,shader,NULL);vkDestroyDescriptorPool(device,pool,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    for(unsigned i=0;i<2;i++)vkDestroySampler(device,samplers[i],NULL);
    vkDestroyImageView(device,view,NULL);vkDestroyImage(device,image,NULL);vkFreeMemory(device,image_memory,NULL);
    for(unsigned i=0;i<4;i++) { vkUnmapMemory(device,allocations[i]);vkDestroyBuffer(device,buffers[i],NULL);vkFreeMemory(device,allocations[i],NULL); }
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);fprintf(stderr,"Vulkan texture coordinates variant%u base%u: %u cases passed\n",variant,mode,tco_count(variant));return 0;
}
