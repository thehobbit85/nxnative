// Alternate the same guest object addresses between two event/report banks.
// Three generations can be queued before an address is recycled. Only a
// public sync wait makes it safe for the CPU to inspect or reuse a bank.
typedef struct {
    uint32_t leading, event, trailing, pad;
    NVNcounterData top, visibility, bottom;
    uint32_t end[4];
} query_results;
typedef struct {
    NVNevent event;
    NVNsync producer, complete;
    query_results *target, *other, saved_other;
    unsigned cookie;
    uint64_t earliest;
} query_generation;

static void finish_query_generation(query_generation *g) {
    NVNsyncWaitResult r=API(nvnSyncWait)(&g->complete,5000000000ull);
    assert(r==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r==NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED);
    assert(API(nvnSyncWait)(&g->producer,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    assert(API(nvnSyncWait)(&g->complete,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    query_results *p=g->target;
    assert(API(nvnEventGetValue)(&g->event)==g->cookie+13 && p->event==g->cookie+13);
    assert(p->top.timestamp>=g->earliest && p->top.timestamp<=p->bottom.timestamp &&
           p->bottom.timestamp<=nvn_timestamp_from_ns(guest_ns()));
    assert(p->top.counter==(DM_GAME_RENDER_COND ? 0 : p->top.timestamp));
    assert(p->bottom.counter==(DM_GAME_RENDER_COND ? 0 : p->bottom.timestamp));
    assert(!p->visibility.counter);
    assert(p->leading==0xa5a5a5a5 && p->trailing==p->leading && p->pad==p->leading);
    for (unsigned i=0;i<4;++i) assert(p->end[i]==p->leading);
    assert(!memcmp(&g->saved_other,g->other,sizeof g->saved_other));
    API(nvnEventFinalize)(&g->event);
    API(nvnSyncFinalize)(&g->producer); API(nvnSyncFinalize)(&g->complete);
}
static void check_query_lifetimes(NVNdevice *device, NVNqueue *first_queue, NVNqueueBuilder *qb) {
    enum { slots=3, rounds=48, results_off=0x4000, commands=0x1000 };
    static _Alignas(16384) uint8_t bytes[0x8000];
    query_generation generations[slots]={0};
    NVNqueue other_queue; assert(API(nvnQueueInitialize)(&other_queue,qb));
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb,device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,bytes,sizeof bytes);
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    assert(API(nvnMemoryPoolInitialize)(&pool,&pb));
    NVNbufferAddress base=API(nvnMemoryPoolGetBufferAddress)(&pool);
    query_results *banks=(query_results *)(bytes+results_off);
    for (unsigned i=0;i<rounds+slots;++i) {
        unsigned slot=i%slots, bank=(i/slots)%2;
        query_generation *g=&generations[slot];
        if (i>=slots) finish_query_generation(g);
        if (i>=rounds) continue;
        unsigned offset=results_off+(slot*2+bank)*sizeof *banks;
        g->target=banks+slot*2+bank; g->other=banks+slot*2+(1-bank);
        g->saved_other=*g->other; g->cookie=0x1000+i;
        memset(g->target,0xa5,sizeof *banks);
        NVNeventBuilder eb;
        API(nvnEventBuilderSetDefaults)(&eb);
        API(nvnEventBuilderSetStorage)(&eb,&pool,offset+offsetof(query_results,event));
        assert(API(nvnEventInitialize)(&g->event,&eb));
        assert(API(nvnSyncInitialize)(&g->producer,device) && API(nvnSyncInitialize)(&g->complete,device));
        assert(API(nvnSyncWait)(&g->complete,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        NVNcommandBuffer cb;
        assert(API(nvnCommandBufferInitialize)(&cb,device));
        API(nvnCommandBufferAddCommandMemory)(&cb,&pool,slot*commands,commands);
        API(nvnCommandBufferBeginRecording)(&cb);
        NVNbufferAddress address=base+offset;
        API(nvnCommandBufferResetCounter)(&cb,NVN_COUNTER_TYPE_SAMPLES_PASSED);
        API(nvnCommandBufferReportCounter)(&cb,NVN_COUNTER_TYPE_TIMESTAMP_TOP,address+offsetof(query_results,top));
        API(nvnCommandBufferSignalEvent)(&cb,&g->event,NVN_EVENT_SIGNAL_MODE_WRITE,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,g->cookie);
        API(nvnCommandBufferReportCounter)(&cb,NVN_COUNTER_TYPE_SAMPLES_PASSED,address+offsetof(query_results,visibility));
        API(nvnCommandBufferFenceSync)(&cb,&g->producer,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
        NVNcommandHandle first=API(nvnCommandBufferEndRecording)(&cb);
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSignalEvent)(&cb,&g->event,NVN_EVENT_SIGNAL_MODE_ADD,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,13);
        API(nvnCommandBufferReportCounter)(&cb,NVN_COUNTER_TYPE_TIMESTAMP,address+offsetof(query_results,bottom));
        API(nvnCommandBufferFenceSync)(&cb,&g->complete,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
        NVNcommandHandle second=API(nvnCommandBufferEndRecording)(&cb);
        NVNqueue *src=(i&1) ? &other_queue : first_queue;
        NVNqueue *dst=(i&1) ? first_queue : &other_queue;
        g->earliest=nvn_timestamp_from_ns(guest_ns());
        API(nvnQueueSubmitCommands)(src,1,&first);
        assert(API(nvnQueueWaitSync)(dst,&g->producer));
        API(nvnQueueSubmitCommands)(dst,1,&second);
        API(nvnCommandBufferFinalize)(&cb);
    }
    API(nvnQueueFinalize)(&other_queue);
    API(nvnMemoryPoolFinalize)(&pool);
    puts("Public NVN query lifetime: 48 generations, 96 queued lists, alternating banks, fresh syncs and preserved report guards");
}
