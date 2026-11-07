// Cache persistence contract without a GPU: corruption/incompatibility must be
// rejected before opaque bytes reach the driver, and failed writes stay dirty.
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#define LOG(...) ((void)0)
static VkDevice g_dev;
static VkPhysicalDeviceProperties g_props;
static unsigned char driver_data[48];
static size_t driver_initial;
static int driver_reject;
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device,const VkPipelineCacheCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator,VkPipelineCache *cache) {
    (void)device;(void)allocator;driver_initial=info->initialDataSize;
    if (driver_reject && info->initialDataSize) return VK_ERROR_INITIALIZATION_FAILED;
    *cache=(VkPipelineCache)(uintptr_t)1;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device,VkPipelineCache cache,size_t *size,void *data) {
    (void)device;(void)cache;
    if (data) { assert(*size>=sizeof driver_data);memcpy(data,driver_data,sizeof driver_data); }
    *size=sizeof driver_data;return VK_SUCCESS;
}
#include "../pipeline_cache.inc"

static void write_byte(size_t offset,unsigned char value) {
    FILE *f=fopen(g_pcache_path,"r+b");assert(f);assert(!fseek(f,offset,SEEK_SET));assert(fwrite(&value,1,1,f)==1);assert(!fclose(f));
}
static void save_fresh(void) { pcache_created();pcache_save();assert(g_pcache_saved==atomic_load(&g_pcache_generation)); }
static void cold_load(void) { g_pcache=VK_NULL_HANDLE;driver_initial=SIZE_MAX;pcache_load();assert(g_pcache); }
int main(void) {
    #ifdef __ANDROID__
    char dir[]="/data/local/tmp/nx-vulkan-cache-XXXXXX";
#else
    char dir[]="/tmp/nx-vulkan-cache-XXXXXX";
#endif
    assert(mkdtemp(dir));assert(!setenv("DM_VK_CACHE_DIR",dir,1));
    g_props.vendorID=0x1234;g_props.deviceID=0x5678;g_props.driverVersion=19;g_props.apiVersion=VK_API_VERSION_1_3;
    for (unsigned i=0;i<VK_UUID_SIZE;i++) g_props.pipelineCacheUUID[i]=(uint8_t)(i*7+3);
    uint32_t header[]={32,VK_PIPELINE_CACHE_HEADER_VERSION_ONE,g_props.vendorID,g_props.deviceID};
    memcpy(driver_data,header,sizeof header);memcpy(driver_data+16,g_props.pipelineCacheUUID,16);memset(driver_data+32,0xa5,16);
    cold_load();assert(driver_initial==0);save_fresh();cold_load();assert(driver_initial==sizeof driver_data);
    write_byte(sizeof(pcache_header)+40,0xff);cold_load();assert(driver_initial==0);save_fresh();
    write_byte(offsetof(pcache_header,driver),20);cold_load();assert(driver_initial==0);save_fresh();
    write_byte(offsetof(pcache_header,chain),'!');cold_load();assert(driver_initial==0);save_fresh();
    write_byte(offsetof(pcache_header,uuid),0);cold_load();assert(driver_initial==0);save_fresh();
    assert(!truncate(g_pcache_path,12));cold_load();assert(driver_initial==0);save_fresh();
    driver_reject=1;cold_load();assert(driver_initial==0);driver_reject=0;
    // A valid envelope containing a foreign Vulkan header must also be ignored.
    driver_data[8]^=1;save_fresh();cold_load();assert(driver_initial==0);driver_data[8]^=1;save_fresh();
    char good[sizeof g_pcache_path];memcpy(good,g_pcache_path,sizeof good);
    snprintf(g_pcache_path,sizeof g_pcache_path,"%s/absent/cache.bin",dir);
    pcache_created();uint64_t previous=g_pcache_saved;pcache_save();assert(g_pcache_saved==previous);
    memcpy(g_pcache_path,good,sizeof good);pcache_save();assert(g_pcache_saved==atomic_load(&g_pcache_generation));
    // A driver upgrade gets a different file instead of overwriting the old one.
    g_props.driverVersion++;cold_load();assert(driver_initial==0 && strcmp(good,g_pcache_path));
    g_props.driverVersion--;g_pcache_ids.deviceUUID[0]=1;cold_load();assert(driver_initial==0 && strcmp(good,g_pcache_path));
    g_pcache_ids.deviceUUID[0]=0;g_pcache_ids.driverUUID[0]=1;cold_load();assert(driver_initial==0 && strcmp(good,g_pcache_path));
    assert(!unlink(good));assert(!rmdir(dir));
    puts("Pipeline cache: warm load, corrupt/truncated/foreign data, driver rejection, failed-write recovery and driver isolation pass");
}
