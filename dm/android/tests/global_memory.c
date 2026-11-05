// Execute exported raw-address shaders using real Vulkan buffer device addresses.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../gfx/nvn/vulkan/shader_contract.h"
#include "global_memory_cases.h"
#define CHECK(call) do { VkResult r_=(call);if(r_) { fprintf(stderr,"%s: %d\n",#call,r_);return 1; } } while(0)
typedef struct { unsigned number,index;VkDescriptorType type; } binding_t;
static int load(VkDevice device,const char *directory,unsigned variant,VkShaderModule *module,binding_t *bindings,unsigned *binding_count) {
    char path[1024],text[8192];int len=snprintf(path,sizeof path,"%s/export/%016llx.vk.txt",directory,0xfeed5000ull+variant);
    if(len<0 || (size_t)len>=sizeof path)return 1;
    FILE *file=fopen(path,"rb");if(!file)return 1;
    size_t size=fread(text,1,sizeof text-1,file);int ok=feof(file)&&!ferror(file);fclose(file);if(!ok)return 1;text[size]=0;
    snprintf(path,sizeof path,"%s/export/%016llx.spv",directory,0xfeed5000ull+variant);
    file=fopen(path,"rb");if(!file)return 1;
    if(fseek(file,0,SEEK_END)) { fclose(file);return 1; }
    long bytes=ftell(file);if(bytes<20 || bytes%4 || fseek(file,0,SEEK_SET)) { fclose(file);return 1; }
    uint32_t *code=malloc((size_t)bytes);if(!code) { fclose(file);return 1; }
    ok=fread(code,1,(size_t)bytes,file)==(size_t)bytes;fclose(file);if(!ok) { free(code);return 1; }
    const char *error=dm_vk_shader_contract(text,size,code,(size_t)bytes/4);
    if(error) { fprintf(stderr,"%s\n",error);free(code);return 1; }
    unsigned seen=0;*binding_count=0;
    for(char *line=strtok(text,"\n");line;line=strtok(NULL,"\n")) {
        unsigned number,count;int reg,off,sreg;char type[8],kind[16],sampler[16];
        if(sscanf(line,"b %u %7s %u %15s %d %d %15s %d",&number,type,&count,kind,&reg,&off,sampler,&sreg)!=8)continue;
        unsigned index=GM_BUFFERS;VkDescriptorType descriptor=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if(!strcmp(kind,"cbuf") && !strcmp(type,"ubo") && reg==0) { index=GM_UNIFORM;descriptor=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; }
        else if(!strcmp(kind,"sbuf_ubo") && !strcmp(type,"ssbo") && reg==0 && (off==0 || off==56))index=off==0?GM_POINTER:GM_OUTPUT;
        else if(!strcmp(kind,"gmem_map") && !strcmp(type,"ssbo")) {
            const char *field=strstr(line," gmem_write=");int writes=-1;
            if(field && sscanf(field," gmem_write=%d",&writes)==1 && writes==(variant>=7))index=GM_MAP;
        } else if(!strcmp(kind,"gmem_log") && !strcmp(type,"ssbo") && variant>=7)index=GM_LOG;
        if(index==GM_BUFFERS || count!=1 || *binding_count==5 || (seen&(1u<<index))) { free(code);return 1; }
        bindings[(*binding_count)++]=(binding_t){number,index,descriptor};seen|=1u<<index;
    }
    unsigned expected=(1u<<GM_POINTER)|(1u<<GM_MAP)|(1u<<GM_OUTPUT);
    if(variant>=7)expected|=(1u<<GM_UNIFORM)|(1u<<GM_LOG);
    if(seen!=expected) { free(code);return 1; }
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
    VkResult result=vkCreateShaderModule(device,&ci,NULL,module);free(code);CHECK(result);return 0;
}
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *memory,uint32_t bits) {
    VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<memory->memoryTypeCount;i++)if((bits&(1u<<i)) && (memory->memoryTypes[i].propertyFlags&flags)==flags)return i;
    fprintf(stderr,"No coherent host memory\n");exit(1);
}
int main(int argc,char **argv) {
    if(argc<2 || argc>3)return 2;int negative=argc==3 && !strcmp(argv[2],"wrong-delta");if(argc==3 && !negative)return 2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice physical;VkResult picked=vkEnumeratePhysicalDevices(instance,&count,&physical);
    if((picked!=VK_SUCCESS && picked!=VK_INCOMPLETE)||!count)return 1;
    VkPhysicalDeviceVulkan12Features f12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&f12};vkGetPhysicalDeviceFeatures2(physical,&features);
    if(!f12.bufferDeviceAddress) { fprintf(stderr,"Raw addresses require bufferDeviceAddress\n");return 77; }
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;if(family==count)return 77;
    float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice device;CHECK(vkCreateDevice(physical,&dci,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    VkBuffer buffers[GM_BUFFERS];VkDeviceMemory allocations[GM_BUFFERS];void *mapped[GM_BUFFERS];
    for(unsigned i=0;i<GM_BUFFERS;i++) {
        VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=gm_sizes[i],.usage=(i?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
        CHECK(vkCreateBuffer(device,&ci,NULL,&buffers[i]));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[i],&req);
        VkMemoryAllocateFlagsInfo flags={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&flags,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits)};
        CHECK(vkAllocateMemory(device,&ai,NULL,&allocations[i]));CHECK(vkBindBufferMemory(device,buffers[i],allocations[i],0));CHECK(vkMapMemory(device,allocations[i],0,VK_WHOLE_SIZE,0,&mapped[i]));
    }
    VkBufferDeviceAddressInfo address_info={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=buffers[GM_POOL]};
    PFN_vkGetBufferDeviceAddress get_address=(PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(device,"vkGetBufferDeviceAddress");if(!get_address)return 77;
    uint64_t gpu_address=get_address(device,&address_info);if(!gpu_address)return 1;
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VkCommandPool commands;CHECK(vkCreateCommandPool(device,&cpci,NULL,&commands));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
    unsigned cases=0;
    for(unsigned variant=0;variant<GM_VARIANTS;variant++) {
        binding_t bindings[5];unsigned n;VkShaderModule shader;if(load(device,argv[1],variant,&shader,bindings,&n))return 1;
        VkDescriptorSetLayoutBinding lb[5];VkDescriptorBufferInfo bi[5];
        for(unsigned i=0;i<n;i++) { lb[i]=(VkDescriptorSetLayoutBinding){bindings[i].number,bindings[i].type,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};bi[i]=(VkDescriptorBufferInfo){buffers[bindings[i].index],0,gm_sizes[bindings[i].index]}; }
        VkDescriptorSetLayoutCreateInfo slci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=n,.pBindings=lb};VkDescriptorSetLayout set_layout;CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layout));
        VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4}};
        VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&pool));
        VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
        VkWriteDescriptorSet writes[5];
        for(unsigned i=0;i<n;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=lb[i].binding,.descriptorCount=1,.descriptorType=lb[i].descriptorType,.pBufferInfo=&bi[i]};vkUpdateDescriptorSets(device,n,writes,0,NULL);
        VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
        VkComputePipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};VkPipeline pipeline;CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
        for(unsigned ci=0;ci<4;ci++)for(unsigned test=0;test<GM_ADDRESSES;test++)for(unsigned mode=0;mode<3;mode++) {
            unsigned pools=gm_counts[ci];if(gm_address(pools,test)%gm_alignment(variant))continue;
            gm_prepare(mapped,gpu_address,variant,pools,test,mode);uint8_t want_pool[1024];uint32_t want_log[1088],want_output[64];gm_expected(mapped,variant,pools,test,want_pool,want_log,want_output);
            if(negative)for(unsigned i=1;i<=pools;i++)((uint64_t*)mapped[GM_MAP])[i*4+2]+=4;
            CHECK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(command,1,1,1);
            VkMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&done,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));
            VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
            if(gm_check(mapped,variant,pools,test,mode,want_pool,want_log,want_output))return 1;cases++;
        }
        vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyShaderModule(device,shader,NULL);vkDestroyDescriptorPool(device,pool,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    }
    vkDestroyCommandPool(device,commands,NULL);
    for(unsigned i=0;i<GM_BUFFERS;i++) { vkUnmapMemory(device,allocations[i]);vkDestroyBuffer(device,buffers[i],NULL);vkFreeMemory(device,allocations[i],NULL); }
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);fprintf(stderr,"Vulkan global memory: %u cases passed\n",cases);return 0;
}
