// Execute the actual Ampere per-program C0 mapping across every resource region.
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include "../../gfx/nvn/vulkan/shader_contract.h"
#include "nvn2_layout_cases.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CHECK(call) do { VkResult r_=(call); if(r_) { fprintf(stderr,"%s: %d\n",#call,r_);return 1; } } while(0)
typedef struct { unsigned storage,driver[3]; } bindings_t;
static int load(VkDevice device,const char *directory,VkShaderModule *module,bindings_t *bindings) {
    char path[1024],text[8192];
    int len=snprintf(path,sizeof path,"%s/export/%016llx.vk.txt",directory,0xfeed9100ull);
    if(len<0 || (size_t)len>=sizeof path)return 1;
    FILE *file=fopen(path,"rb");if(!file)return 1;
    size_t size=fread(text,1,sizeof text-1,file);int ok=feof(file)&&!ferror(file);fclose(file);if(!ok)return 1;text[size]=0;
    len=snprintf(path,sizeof path,"%s/export/%016llx.spv",directory,0xfeed9100ull);
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
        if(!strcmp(kind,"sbuf_ubo")&&!strcmp(type,"ssbo")&&count==1&&reg==0&&off==56) { bindings->storage=binding;seen|=8; }
        else if(!strcmp(kind,"drvcb")&&!strcmp(type,"ubo")&&count==1&&(off==0||off==2||off==24)) { unsigned index=off==0?0:off==2?1:2;bindings->driver[index]=binding;seen|=1u<<index; }
        else { free(code);return 1; }
    }
    if(seen!=15) { free(code);return 1; }
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
    VkResult result=vkCreateShaderModule(device,&ci,NULL,module);free(code);CHECK(result);return 0;
}
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *memory,uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memory->memoryTypeCount;i++) if((bits&(1u<<i)) && (memory->memoryTypes[i].propertyFlags&flags)==flags)return i;
    fprintf(stderr,"No compatible memory type\n");exit(1);
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice physical;VkResult picked=vkEnumeratePhysicalDevices(instance,&count,&physical);
    if((picked!=VK_SUCCESS && picked!=VK_INCOMPLETE)||!count)return 1;
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(physical,&features);
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;if(family==count)return 77;
    float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pEnabledFeatures=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice device;CHECK(vkCreateDevice(physical,&dci,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    bindings_t bindings={0};VkShaderModule shader;int loaded=load(device,argv[1],&shader,&bindings);if(loaded)return loaded;
    VkBuffer buffers[2];VkDeviceMemory allocations[2];uint32_t *mapped[2];const unsigned sizes[]={8448,1024};
    for(unsigned i=0;i<2;i++) {
        VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizes[i],.usage=i==1?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        CHECK(vkCreateBuffer(device,&ci,NULL,&buffers[i]));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[i],&req);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device,&ai,NULL,&allocations[i]));CHECK(vkBindBufferMemory(device,buffers[i],allocations[i],0));CHECK(vkMapMemory(device,allocations[i],0,VK_WHOLE_SIZE,0,(void**)&mapped[i]));
    }
    unsigned binding_count=4;
    VkDescriptorSetLayoutBinding lb[4];
    for(unsigned i=0;i<3;i++)lb[i]=(VkDescriptorSetLayoutBinding){bindings.driver[i],VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    lb[3]=(VkDescriptorSetLayoutBinding){bindings.storage,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo slci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=binding_count,.pBindings=lb};
    VkDescriptorSetLayout set_layout;CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layout));
    VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,3},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};
    VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&pool));
    VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
    VkDescriptorBufferInfo bi[]={{buffers[0],0,4096},{buffers[0],4096,4096},{buffers[0],8192,256},{buffers[1],256,NL_WORDS*4}};
    VkWriteDescriptorSet writes[4];
    for(unsigned i=0;i<binding_count;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=lb[i].binding,.descriptorCount=1,.descriptorType=lb[i].descriptorType,.pBufferInfo=&bi[i]};
    vkUpdateDescriptorSets(device,binding_count,writes,0,NULL);
    VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
    VkComputePipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};
    VkPipeline pipeline;CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VkCommandPool commands;CHECK(vkCreateCommandPool(device,&cpci,NULL,&commands));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    for(unsigned test=0;test<NL_CASES;test++) {
        for(unsigned i=0;i<1024;i++) { mapped[0][i]=nl_word(0,i*4,test);mapped[0][1024+i]=nl_word(2,i*4,test); }
        for(unsigned i=0;i<64;i++)mapped[0][2048+i]=nl_word(24,i*4,test);memset(mapped[1],0xcc,sizes[1]);
        CHECK(vkResetCommandBuffer(command,0));CHECK(vkBeginCommandBuffer(command,&begin));vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(command,1,1,1);
        VkMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&done,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
        if(nl_check(test,mapped[1]))return 1;
    }
    vkDestroyCommandPool(device,commands,NULL);vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyShaderModule(device,shader,NULL);vkDestroyDescriptorPool(device,pool,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    for(unsigned i=0;i<2;i++) { vkUnmapMemory(device,allocations[i]);vkDestroyBuffer(device,buffers[i],NULL);vkFreeMemory(device,allocations[i],NULL); }
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);fprintf(stderr,"Vulkan NVN2 layout:16 cases passed\n");return 0;
}
