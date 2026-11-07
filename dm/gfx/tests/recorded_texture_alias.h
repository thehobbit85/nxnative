// NVN texture objects describe storage. Exact aliases and IMAGE-only variants
// must share GPU contents, including when all objects are rebuilt on that pool.
static void check_texture_object_aliases(NVNdevice *device,NVNqueue *queue) {
    enum { W=8,H=8,BYTES=W*H*4 };
    static _Alignas(16384) uint8_t storage[0x4000],commands[0x10000];
    NVNmemoryPoolBuilder pb;NVNmemoryPool pool,command_pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);API(nvnMemoryPoolBuilderSetDevice)(&pb,device);
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,commands,sizeof commands);assert(API(nvnMemoryPoolInitialize)(&command_pool,&pb));
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,storage,sizeof storage);assert(API(nvnMemoryPoolInitialize)(&pool,&pb));
    NVNbufferAddress address=API(nvnMemoryPoolGetBufferAddress)(&command_pool);
    NVNtextureBuilder tb;NVNtexture textures[2];
    API(nvnTextureBuilderSetDefaults)(&tb);API(nvnTextureBuilderSetDevice)(&tb,device);
    API(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_2D);API(nvnTextureBuilderSetSize2D)(&tb,W,H);
    API(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);API(nvnTextureBuilderSetStorage)(&tb,&pool,0);
    for (int i=0;i<2;i++) { API(nvnTextureBuilderSetFlags)(&tb,i ? NVN_TEXTURE_FLAGS_IMAGE : 0);assert(API(nvnTextureInitialize)(&textures[i],&tb)); }
    NVNcommandBuffer cb;assert(API(nvnCommandBufferInitialize)(&cb,device));
    API(nvnCommandBufferAddCommandMemory)(&cb,&command_pool,0,0x3000);
    NVNcopyRegion full={0,0,0,W,H,1};
    for (int phase=0;phase<4;phase++) {
        if (phase==2) {
            for (int i=0;i<2;i++) API(nvnTextureFinalize)(&textures[i]);
            for (int i=0;i<2;i++) { API(nvnTextureBuilderSetFlags)(&tb,i ? NVN_TEXTURE_FLAGS_IMAGE : 0);assert(API(nvnTextureInitialize)(&textures[i],&tb)); }
        }
        if (phase==3) {
            memset(storage,0,sizeof storage);assert(API(nvnMemoryPoolInitialize)(&pool,&pb));
            for (int i=0;i<2;i++) { API(nvnTextureBuilderSetFlags)(&tb,i ? NVN_TEXTURE_FLAGS_IMAGE : 0);assert(API(nvnTextureInitialize)(&textures[i],&tb)); }
        }
        memset(commands+0x4000,0xcc,BYTES);
        API(nvnCommandBufferBeginRecording)(&cb);
        if (phase<2) {
            const NVNtexture *target[]={&textures[phase]};
            API(nvnCommandBufferSetRenderTargets)(&cb,1,target,NULL,NULL,NULL);
            API(nvnCommandBufferSetScissor)(&cb,0,0,W,H);
            API(nvnCommandBufferClearColor)(&cb,0,(float[]){phase ? 0 : 1,phase ? 1 : 0,0,1},15);
        }
        API(nvnCommandBufferSetCopyRowStride)(&cb,0);API(nvnCommandBufferSetCopyImageStride)(&cb,0);
        API(nvnCommandBufferCopyTextureToBuffer)(&cb,&textures[phase==0 ? 1 : 0],NULL,&full,address+0x4000,0);
        NVNcommandHandle handle=API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue,1,&handle);
        if (phase==2) {
            // Retire objects and their pool after encoding but before waiting
            // for completion. The readback still owns its image until its fence.
            for (int i=0;i<2;i++) API(nvnTextureFinalize)(&textures[i]);
            API(nvnMemoryPoolFinalize)(&pool);
        }
        API(nvnQueueFinish)(queue);
        uint8_t expected[]={phase==0 ? 255 : 0,phase==1 || phase==2 ? 255 : 0,0,phase==3 ? 0 : 255};
        for (int pixel=0;pixel<W*H;pixel++) if (memcmp(commands+0x4000+4*pixel,expected,4)) {
            const uint8_t *got=commands+0x4000+4*pixel;
            die("texture alias phase%d pixel%d: %u,%u,%u,%u expected %u,%u,%u,%u",phase,pixel,
                got[0],got[1],got[2],got[3],expected[0],expected[1],expected[2],expected[3]);
        }
        if (phase==0) {
            // This descriptor has never been bound to the GPU. CPU readback
            // must still recognize the storage's GPU owner.
            NVNtexture unused;assert(API(nvnTextureInitialize)(&unused,&tb));
            uint8_t pixel[4];NVNcopyRegion one={0,0,0,1,1,1};
            API(nvnTextureReadTexels)(&unused,NULL,&one,pixel);assert(!memcmp(pixel,expected,4));
            API(nvnTextureFinalize)(&unused);
        }
    }
    API(nvnCommandBufferFinalize)(&cb);for (int i=0;i<2;i++) API(nvnTextureFinalize)(&textures[i]);
    API(nvnMemoryPoolFinalize)(&pool);API(nvnMemoryPoolFinalize)(&command_pool);
    puts("Public texture aliases preserved GPU writes, IMAGE-only descriptors, object rebuilds, unused-alias CPU reads, retirement before completion and pool reuse");
}

// CPU uploads with explicit packaged layouts share one weakly held image.
// Later GPU writes must be visible through every already-resolved descriptor.
static void check_texture_packaged_aliases(NVNdevice *device,NVNqueue *queue) {
    enum { W=8,H=8,BYTES=W*H*4 };
    static _Alignas(16384) uint8_t storage[0x4000],commands[0x10000];
    memset(storage,0,sizeof storage);
    NVNmemoryPoolBuilder pb;NVNmemoryPool pool,command_pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);API(nvnMemoryPoolBuilderSetDevice)(&pb,device);
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,commands,sizeof commands);assert(API(nvnMemoryPoolInitialize)(&command_pool,&pb));
    API(nvnMemoryPoolBuilderSetStorage)(&pb,storage,sizeof storage);assert(API(nvnMemoryPoolInitialize)(&pool,&pb));
    NVNbufferAddress address=API(nvnMemoryPoolGetBufferAddress)(&command_pool);
    NVNtextureBuilder tb;NVNtexture textures[3];NVNpackagedTextureLayout layout={0};
    API(nvnTextureBuilderSetDefaults)(&tb);API(nvnTextureBuilderSetDevice)(&tb,device);
    API(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_2D);API(nvnTextureBuilderSetSize2D)(&tb,W,H);
    API(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);API(nvnTextureBuilderSetStorage)(&tb,&pool,0);
    API(nvnTextureBuilderSetPackagedTextureLayout)(&tb,&layout);
    for (int i=0;i<3;i++) assert(API(nvnTextureInitialize)(&textures[i],&tb));
    NVNcommandBuffer cb;assert(API(nvnCommandBufferInitialize)(&cb,device));
    API(nvnCommandBufferAddCommandMemory)(&cb,&command_pool,0,0x3000);
    NVNcopyRegion full={0,0,0,W,H,1};uint8_t pixels[BYTES];
    for (int phase=0;phase<4;phase++) {
        uint8_t expected[]={phase==0 ? 255 : 0,phase>=2 ? 255 : 0,phase==1 ? 255 : 0,255};
        if (phase<2) {
            for (int j=0;j<W*H;j++) memcpy(pixels+4*j,expected,4);
            API(nvnTextureWriteTexels)(&textures[phase ? 2 : 0],NULL,&full,pixels);
        }
        if (phase==3) { API(nvnTextureFinalize)(&textures[1]);API(nvnTextureFinalize)(&textures[2]); }
        API(nvnCommandBufferBeginRecording)(&cb);
        if (phase==2) {
            const NVNtexture *targets[]={&textures[1]};
            API(nvnCommandBufferSetRenderTargets)(&cb,1,targets,NULL,NULL,NULL);
            API(nvnCommandBufferSetScissor)(&cb,0,0,W,H);
            API(nvnCommandBufferClearColor)(&cb,0,(float[]){0,1,0,1},15);
        }
        API(nvnCommandBufferSetCopyRowStride)(&cb,0);API(nvnCommandBufferSetCopyImageStride)(&cb,0);
        int readers=phase==3 ? 1 : 2;
        for (int i=0;i<readers;i++) API(nvnCommandBufferCopyTextureToBuffer)(&cb,&textures[i],NULL,&full,address+0x4000+i*BYTES,0);
        NVNcommandHandle list=API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue,1,&list);API(nvnQueueFinish)(queue);
        for (int i=0;i<readers;i++) for (int pixel=0;pixel<W*H;pixel++) {
            uint8_t *got=commands+0x4000+i*BYTES+4*pixel;
            if (memcmp(got,expected,4)) die("packaged alias phase%d reader%d pixel%d: %u,%u,%u,%u expected %u,%u,%u,%u",
                phase,i,pixel,got[0],got[1],got[2],got[3],expected[0],expected[1],expected[2],expected[3]);
        }
    }
    API(nvnCommandBufferFinalize)(&cb);API(nvnTextureFinalize)(&textures[0]);
    API(nvnMemoryPoolFinalize)(&pool);API(nvnMemoryPoolFinalize)(&command_pool);
    puts("Public packaged aliases shared CPU updates, GPU writes and independent object lifetimes");
}
