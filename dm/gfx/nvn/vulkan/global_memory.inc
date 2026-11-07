// Raw-pointer shaders share a command's ordinary storage backing, including
// overlapping descriptors and GPU writes already recorded in this submission.
static pool_range g_raw_pools[DMSH_GMEM_MAP_ENTRIES-1];static size_t g_nraw_pools;
static VkDescriptorBufferInfo g_raw_map,g_raw_log;
static uint32_t *g_raw_log_words;
static gw_t g_raw_sources[DMSH_GMEM_MAP_ENTRIES];static unsigned g_raw_source_count;
static int g_raw_writes;
static int raw_pool_order(const void *a,const void *b) {
    uintptr_t first=(uintptr_t)((const pool_range *)a)->mem,second=(uintptr_t)((const pool_range *)b)->mem;
    return (first>second)-(first<second);
}
static int collect_global_memory(shader_t *const *stages,size_t stage_count) {
    g_nraw_pools=0;g_raw_map=(VkDescriptorBufferInfo){0};g_raw_log=(VkDescriptorBufferInfo){0};
    g_raw_writes=0;g_raw_source_count=0;g_raw_log_words=NULL;
    int needed=0;
    for (size_t stage=0;stage<stage_count;stage++) if (stages[stage]) {
        g_raw_writes|=stages[stage]->raw_writes;
        for (int i=0;i<stages[stage]->nb;i++) needed|=stages[stage]->b[i].kind==K_GMEM_MAP;
    }
    if (!needed) return 1;
    pthread_mutex_lock(&g_pool_mu);
    for (size_t i=0;i<g_npool;i++) {
        const pool_range *pool=&g_pool[i];
        if (!pool->addressed || !pool->size) continue;
        if (g_nraw_pools==DMSH_GMEM_MAP_ENTRIES-1 || g_nstorage==MAX_BUFFER_BINDINGS) {
            pthread_mutex_unlock(&g_pool_mu);RED_ONCE(140,"raw memory pool table capacity exceeded");return 0;
        }
        g_raw_pools[g_nraw_pools++]=*pool;
        uint64_t address=(uintptr_t)pool->mem;
        g_storage[g_nstorage++]=(storage_range){.addr=address,.size=pool->size,.texel=address&~UINT64_C(3),.texel_align=4,.writable=g_raw_writes};
    }
    pthread_mutex_unlock(&g_pool_mu);
    qsort(g_raw_pools,g_nraw_pools,sizeof *g_raw_pools,raw_pool_order);
    return 1;
}
static int global_memory_map(VkDescriptorBufferInfo *binding) {
    if (g_raw_map.buffer) { *binding=g_raw_map;return 1; }
    static uint64_t map[DMSH_GMEM_MAP_ENTRIES][4];
    memset(map,0,sizeof map);
    for (unsigned i=1;i<DMSH_GMEM_MAP_ENTRIES;i++) map[i][0]=UINT64_MAX;
    unsigned entry=1;uint64_t previous_end=0;
    for (size_t i=0;i<g_nraw_pools;i++) {
        const pool_range *pool=&g_raw_pools[i];
        uint64_t start=(uintptr_t)pool->mem,end=start+pool->size;
        if (start<previous_end) start=previous_end;
        if (start>=end) continue;
        VkBuffer buffer;VkDeviceSize offset;uint8_t *mapped;
        if (!prepared_storage(start,end-start,&buffer,&offset,&mapped)) return 0;
        VkBufferDeviceAddressInfo info={VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,NULL,buffer};
        uint64_t device=p_buffer_address(g_dev,&info);
        if (!device) return 0;
        g_raw_sources[entry]=(gw_t){.addr=start,.size=end-start,.buf=buffer,.off=offset,.p=mapped};
        map[entry][0]=start;map[entry][1]=end;map[entry][2]=device+offset-start;
        uint64_t chunk=UINT64_C(1)<<DMSH_GMEM_HINT_SHIFT;
        uint64_t first=(start+chunk-1)>>DMSH_GMEM_HINT_SHIFT,last=end>>DMSH_GMEM_HINT_SHIFT;
        for (uint64_t part=first;part<last && part<(1u<<(32-13));part++)
            map[part%DMSH_GMEM_MAP_ENTRIES][3]=(part<<13)|entry;
        entry++;previous_end=end;
    }
    g_raw_source_count=entry;
    VkDeviceSize offset=ring_put(map,sizeof map,g_props.limits.minStorageBufferOffsetAlignment);
    if (offset==VK_WHOLE_SIZE) return 0;
    g_raw_map=(VkDescriptorBufferInfo){C->ring,offset,sizeof map};*binding=g_raw_map;
    return 1;
}

// Current DMSH shaders log the selected map entry, one bit per range.
static int global_memory_log(VkDescriptorBufferInfo *binding) {
    if (!g_raw_log.buffer) {
        const VkDeviceSize bytes=DMSH_GMEM_MAP_ENTRIES/8;
        VkDeviceSize offset=ring_alloc(bytes,g_props.limits.minStorageBufferOffsetAlignment);
        if (offset==VK_WHOLE_SIZE) return 0;
        g_raw_log_words=(uint32_t *)(C->ringp+offset);memset(g_raw_log_words,0,bytes);
        g_raw_log=(VkDescriptorBufferInfo){C->ring,offset,bytes};
    }
    *binding=g_raw_log;return 1;
}
static int global_memory_written(void) {
    if (!g_raw_writes || !g_raw_log_words || !g_raw_map.buffer) return 0;
    for (unsigned entry=1;entry<g_raw_source_count;entry++) {
        const gw_t *source=&g_raw_sources[entry];
        // Later GPU consumers conservatively share every reachable range.
        // Completion copies only ranges whose bits were set by actual stores.
        gw_add(source->addr,source->size,source->buf,source->off,source->p,g_fence+1,0);
        post_add((post_t){.kind=POST_GMEM_COPY,.dst=(void *)(uintptr_t)source->addr,.mode=entry/32,
            .v=1u<<(entry%32),.addr=(uintptr_t)g_raw_log_words,.src=source->p,.n=source->size});
    }
    return 1;
}
