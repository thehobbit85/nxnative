static void cap_head(void) {
    cap_buf head={0};char line[2048];
#define CAP_KV(...) do { int length=snprintf(line,sizeof line,__VA_ARGS__); if (length>0) cap_buf_put(&head,line,CAP_MIN((size_t)length,sizeof line-1)); } while (0)
    CAP_KV("format\t%u\nbackend\tvulkan\niface\t%d\nchain\t%s\ntitle\t%s\n",CAP_VERSION,CAP_VK_IFACE,DMSH_CHAIN,DM_GAME_ID);
    CAP_KV("frame\t%d\nframes\t1\n",g_cap_frame);
    CAP_KV("device\t%s\nvendor\t%u\ndriver\t%u\n",g_props.deviceName,g_props.vendorID,g_props.driverVersion);
    extern char **environ;
    for (char **entry=environ;*entry;entry++)
        if (!strncmp(*entry,"DM_",3) && strncmp(*entry,"DM_NVN_CAPTURE=",15)) CAP_KV("env\t%s\n",*entry);
#undef CAP_KV
    cap_put(&g_cap_w,CAP_HEAD,0,head.p,head.n,0);cap_buf_free(&head);
    const char *layout=cap_layout();cap_put(&g_cap_w,CAP_LAYT,0,layout,strlen(layout),0);
    snprintf(line,sizeof line,"tex_pool_n\t%d\norigin_ll\t%d\nqueue_origin_ll\t%d\ndepth_zero\t%d\n",
        g_ntreg,atomic_load(&g_origin_ll),g_queue_origin_ll,g_depth_zero);
    cap_put(&g_cap_w,CAP_STAT,0,line,strlen(line),0);
}

static int cap_pool_order(const void *left,const void *right) {
    uintptr_t a=(uintptr_t)((const pool_range *)left)->mem,b=(uintptr_t)((const pool_range *)right)->mem;
    return (a>b)-(a<b);
}
static void cap_pools(uint32_t submit) {
    pthread_mutex_lock(&g_pool_mu);
    size_t count=g_npool;
    pool_range *pools=malloc(CAP_MAX(count,1u)*sizeof *pools);
    if (!pools) { pthread_mutex_unlock(&g_pool_mu);cap_fail("cannot allocate pool snapshot");return; }
    memcpy(pools,g_pool,count*sizeof *pools);
    pthread_mutex_unlock(&g_pool_mu);
    qsort(pools,count,sizeof *pools,cap_pool_order);
    for (size_t i=0;i<count;) {
        uint64_t lo=(uintptr_t)pools[i].mem,hi=lo+pools[i].size;
        size_t end=i+1;
        while (end<count && (uintptr_t)pools[end].mem<=hi) {
            hi=CAP_MAX(hi,(uintptr_t)pools[end].mem+pools[end].size);end++;
        }
        if (hi>lo && nvn_host_readable(lo,hi-lo)) {
            int region=cap_reg_find(lo);
            if (region<0 || g_cap_reg[region].lo+g_cap_reg[region].size<hi) {
                if (region>=0 || cap_reg_find(hi-1)>=0) cap_fail("pool grew over a captured region");
                else cap_reg_add(lo,hi-lo,0);
            }
        } else cap_fail("pool storage is unreadable or sparse");
        i=end;
    }
    cap_buf list={0};
    for (size_t i=0;i<count;i++) {
        cap_padd pool={(uintptr_t)pools[i].mem,pools[i].size};
        cap_enc_addr(NULL,&pool.mem,0,0);cap_buf_put(&list,&pool,sizeof pool);
    }
    cap_put(&g_cap_w,CAP_PADD,submit,list.p,list.n,1);
    cap_buf_free(&list);free(pools);
}
