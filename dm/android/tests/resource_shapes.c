// Execute exported resource interfaces, including array and row boundaries.
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include "../../gfx/nvn/vulkan/shader_contract.h"
#include "resource_shapes_cases.h"
#define CHECK(x) do { VkResult r_=(x);if(r_) { fprintf(stderr,"%s: %d\n",#x,r_);return 1; } } while(0)
typedef struct { VkBuffer buffer;VkDeviceMemory memory;uint32_t *data;unsigned size; } buffer_t;
typedef struct { VkImage image;VkDeviceMemory memory;VkImageView view; } image_t;
typedef struct { unsigned binding,count;char type[8],kind[16];int reg,off; } binding_t;
static uint32_t memory_type(VkPhysicalDeviceMemoryProperties m,uint32_t bits,VkMemoryPropertyFlags flags) {
    for(unsigned i=0;i<m.memoryTypeCount;i++)if((bits&(1u<<i))&&(m.memoryTypes[i].propertyFlags&flags)==flags)return i;
    fprintf(stderr,"No memory type\n");exit(1);
}
static int buffer_new(VkDevice d,VkPhysicalDeviceMemoryProperties m,buffer_t *b,unsigned size) {
    b->size=size;
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    CHECK(vkCreateBuffer(d,&ci,NULL,&b->buffer));VkMemoryRequirements r;vkGetBufferMemoryRequirements(d,b->buffer,&r);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=memory_type(m,r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(d,&ai,NULL,&b->memory));CHECK(vkBindBufferMemory(d,b->buffer,b->memory,0));CHECK(vkMapMemory(d,b->memory,0,VK_WHOLE_SIZE,0,(void**)&b->data));return 0;
}
static int image_new(VkDevice d,VkPhysicalDeviceMemoryProperties m,image_t *im,unsigned width,unsigned height) {
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R32_SFLOAT,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(d,&ci,NULL,&im->image));VkMemoryRequirements r;vkGetImageMemoryRequirements(d,im->image,&r);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=memory_type(m,r.memoryTypeBits,0)};
    CHECK(vkAllocateMemory(d,&ai,NULL,&im->memory));CHECK(vkBindImageMemory(d,im->image,im->memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=im->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32_SFLOAT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK(vkCreateImageView(d,&vi,NULL,&im->view));return 0;
}
static int load(VkDevice d,const char *directory,VkShaderModule *shader,binding_t *bindings,unsigned *count) {
    char path[1024],text[32768];snprintf(path,sizeof path,"%s/export/00000000feed6100.vk.txt",directory);FILE *file=fopen(path,"rb");if(!file)return 1;
    size_t n=fread(text,1,sizeof text-1,file);int ok=feof(file)&&!ferror(file);fclose(file);if(!ok)return 1;text[n]=0;
    snprintf(path,sizeof path,"%s/export/00000000feed6100.spv",directory);file=fopen(path,"rb");if(!file)return 1;
    if(fseek(file,0,SEEK_END)) { fclose(file);return 1; }long bytes=ftell(file);if(bytes<20||bytes%4||fseek(file,0,SEEK_SET)) { fclose(file);return 1; }
    uint32_t *code=malloc((size_t)bytes);if(!code) { fclose(file);return 1; }ok=fread(code,1,(size_t)bytes,file)==(size_t)bytes;fclose(file);if(!ok) { free(code);return 1; }
    const char *error=dm_vk_shader_contract(text,n,code,(size_t)bytes/4);if(error) { fprintf(stderr,"%s\n",error);free(code);return 1; }
    *count=0;for(char *line=strtok(text,"\n");line;line=strtok(NULL,"\n"))if(!strncmp(line,"b ",2)) {
        if(*count==DM_VK_MAX_BINDINGS) { free(code);return 1; }binding_t *b=&bindings[(*count)++];
        if(sscanf(line,"b %u %7s %u %15s %d %d",&b->binding,b->type,&b->count,b->kind,&b->reg,&b->off)!=6) { free(code);return 1; }
    }
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};VkResult r=vkCreateShaderModule(d,&ci,NULL,shader);free(code);CHECK(r);return 0;
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;unsigned mode=(unsigned)strtoul(argv[2],NULL,10);if(mode>RS_MANY)return 2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t n=1;VkPhysicalDevice pd;VkResult picked=vkEnumeratePhysicalDevices(instance,&n,&pd);if((picked!=VK_SUCCESS&&picked!=VK_INCOMPLETE)||!n)return 77;
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(pd,&memory);VkPhysicalDeviceDescriptorIndexingFeatures indexing={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&indexing};vkGetPhysicalDeviceFeatures2(pd,&features);
    if(mode==RS_SLOTS&&(!indexing.shaderUniformTexelBufferArrayNonUniformIndexing||!indexing.shaderSampledImageArrayNonUniformIndexing))return 77;
    VkQueueFamilyProperties families[32];n=32;vkGetPhysicalDeviceQueueFamilyProperties(pd,&n,families);unsigned family=0;while(family<n&&!(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;if(family==n)return 77;
    float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.pEnabledFeatures=&features.features,.pNext=&indexing};VkDevice d;CHECK(vkCreateDevice(pd,&dci,NULL,&d));VkQueue queue;vkGetDeviceQueue(d,family,0,&queue);
    binding_t bindings[DM_VK_MAX_BINDINGS];VkShaderModule shader;unsigned nb;if(load(d,argv[1],&shader,bindings,&nb))return 1;
    buffer_t buffers[30]={0};for(unsigned i=0;i<30;i++)if(buffer_new(d,memory,&buffers[i],i<10?65536:i==29?RS_WIDTH*RS_HEIGHT*4:32768))return 1;
    // 0..9 uniform slots, 10..25 storage slots, 26 input handles, 27 output, 28 support, 29 texture data.
    unsigned ni=mode==RS_MANY?40:1,width=mode==RS_MANY?1:RS_WIDTH,height=mode==RS_MANY?1:RS_HEIGHT;
    image_t images[40]={0};for(unsigned i=0;i<ni;i++)if(image_new(d,memory,&images[i],width,height))return 1;
    VkSamplerCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};VkSampler sampler;CHECK(vkCreateSampler(d,&sci,NULL,&sampler));
    VkBufferView views[32];for(unsigned i=0;i<32;i++) {
        for(unsigned x=0;x<256;x++)buffers[29].data[i*256+x]=rs_texel(RS_SLOTS,i,x,0);
        VkBufferViewCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=buffers[29].buffer,.format=VK_FORMAT_R32_SFLOAT,.offset=i*1024,.range=1024};CHECK(vkCreateBufferView(d,&ci,NULL,&views[i]));
    }
    VkDescriptorSetLayoutBinding lb[DM_VK_MAX_BINDINGS];VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,128},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,128},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,128},{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,128},{VK_DESCRIPTOR_TYPE_SAMPLER,128}};
    for(unsigned i=0;i<nb;i++) {
        VkDescriptorType type=!strcmp(bindings[i].type,"ubo")?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:!strcmp(bindings[i].type,"ssbo")?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:!strcmp(bindings[i].type,"cis")?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:!strcmp(bindings[i].type,"utb")?VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:VK_DESCRIPTOR_TYPE_SAMPLER;
        lb[i]=(VkDescriptorSetLayoutBinding){bindings[i].binding,type,bindings[i].count,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    }
    VkDescriptorSetLayoutCreateInfo slci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=nb,.pBindings=lb};VkDescriptorSetLayout sl;CHECK(vkCreateDescriptorSetLayout(d,&slci,NULL,&sl));
    VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=5,.pPoolSizes=sizes};VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(d,&dpci,NULL,&pool));
    VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&sl};VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(d,&dsai,&set));
    for(unsigned i=0;i<nb;i++) {
        binding_t b=bindings[i];unsigned index=0;VkDeviceSize offset=0;
        if(!strcmp(b.kind,"cbuf")&&b.reg>=0&&b.reg<10)index=(unsigned)b.reg;
        else if(!strcmp(b.kind,"sbuf")&&b.reg>=0&&b.reg<16) { index=10+(unsigned)b.reg;offset=256; }
        else if(!strcmp(b.kind,"sbuf_ubo")&&(b.off==0||b.off==56)) { index=b.off?27:26;offset=256; }
        else if(!strcmp(b.kind,"support"))index=28;
        else if(strcmp(b.kind,"tex")&&strcmp(b.kind,"tex_slots")&&strcmp(b.kind,"samp_slots"))return 1;
        VkDescriptorBufferInfo bi={buffers[index].buffer,offset,offset?32:buffers[index].size};
        VkDescriptorImageInfo ii[32];for(unsigned j=0;j<32;j++)ii[j]=(VkDescriptorImageInfo){sampler,images[mode==RS_MANY?(unsigned)b.reg:0].view,VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=b.binding,.descriptorCount=b.count,.descriptorType=lb[i].descriptorType,.pBufferInfo=&bi,.pImageInfo=ii,.pTexelBufferView=views};vkUpdateDescriptorSets(d,1,&w,0,NULL);
    }
    VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&sl};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(d,&plci,NULL,&layout));
    VkComputePipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};VkPipeline pipeline;CHECK(vkCreateComputePipelines(d,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VkCommandPool commands;CHECK(vkCreateCommandPool(d,&cpci,NULL,&commands));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(d,&cbai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    for(unsigned test=0;test<rs_count(mode);test++) {
        for(unsigned i=0;i<10;i++)rs_uniform(buffers[i].data,mode,i,test);
        for(unsigned i=10;i<29;i++)memset(buffers[i].data,0xcc,buffers[i].size);
        memset(buffers[28].data,0,buffers[28].size);for(unsigned i=0;i<73*4;i++)buffers[28].data[48+i]=rs_bits(1);
        for(unsigned i=0;i<16;i++)buffers[10+i].data[64]=rs_storage(i,test);
        for(unsigned i=0;i<8;i++)buffers[26].data[64+i]=rs_slot(test,i);
        CHECK(vkResetCommandBuffer(command,0));CHECK(vkBeginCommandBuffer(command,&begin));
        if(mode!=RS_SLOTS && (test==0||mode==RS_MANY)) {
            for(unsigned i=0;i<ni;i++) {
                for(unsigned x=0;x<width*height;x++)buffers[29].data[i*width*height+x]=rs_texel(mode,i,x,test);
                VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=test?VK_ACCESS_SHADER_READ_BIT:0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=test?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=images[i].image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
                vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
                VkBufferImageCopy copy={.bufferOffset=i*width*height*4,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={width,height,1}};vkCmdCopyBufferToImage(command,buffers[29].buffer,images[i].image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
            }
            VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);
        }
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(command,1,1,1);
        VkMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&done,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
        if(rs_check(mode,test,buffers[mode==RS_MANY?25:27].data))return 1;
    }
    vkDestroyCommandPool(d,commands,NULL);vkDestroyPipeline(d,pipeline,NULL);vkDestroyPipelineLayout(d,layout,NULL);vkDestroyShaderModule(d,shader,NULL);vkDestroyDescriptorPool(d,pool,NULL);vkDestroyDescriptorSetLayout(d,sl,NULL);vkDestroySampler(d,sampler,NULL);
    for(unsigned i=0;i<32;i++)vkDestroyBufferView(d,views[i],NULL);
    for(unsigned i=0;i<ni;i++) { vkDestroyImageView(d,images[i].view,NULL);vkDestroyImage(d,images[i].image,NULL);vkFreeMemory(d,images[i].memory,NULL); }
    for(unsigned i=0;i<30;i++) { vkUnmapMemory(d,buffers[i].memory);vkDestroyBuffer(d,buffers[i].buffer,NULL);vkFreeMemory(d,buffers[i].memory,NULL); }
    vkDestroyDevice(d,NULL);vkDestroyInstance(instance,NULL);fprintf(stderr,"Vulkan resource mode%u: %u dispatches passed\n",mode,rs_count(mode));return 0;
}
