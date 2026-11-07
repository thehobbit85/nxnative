#ifdef __ANDROID__
// Public queue coverage for encoded BC writes through virtual physical aliases.
static void check_sparse_encoded_writes(NVNdevice *device,NVNqueue *queue) {
    enum { PAGE=0x4000,W=256,H=256,BLOCKS=W*H/2,PIXELS=W*H*4 };
    static _Alignas(16384) uint8_t local[0xe0000];
    uint8_t *physical=mmap(NULL,2*PAGE,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANON,-1,0);
    assert(physical!=MAP_FAILED);
    for (unsigned i=0;i<2*PAGE;i+=8) memcpy(physical+i,(uint8_t[]){0x1f,0,0,0,0,0,0,0},8);
    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool,backing,sparse;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);API(nvnMemoryPoolBuilderSetDevice)(&pb,device);
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_PHYSICAL|NVN_MEMORY_POOL_FLAGS_CPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,physical,2*PAGE);assert(API(nvnMemoryPoolInitialize)(&backing,&pb));
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,NULL,2*BLOCKS);assert(API(nvnMemoryPoolInitialize)(&sparse,&pb));
    for (unsigned i=0;i<2;i++) {
        map_req map={.phys=&backing,.voff=i*BLOCKS,.size=PAGE};
        assert(API(nvnMemoryPoolMapVirtual)(&sparse,1,(const NVNmappingRequest *)&map));
    }
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,local,sizeof local);assert(API(nvnMemoryPoolInitialize)(&pool,&pb));
    NVNbufferAddress address=API(nvnMemoryPoolGetBufferAddress)(&pool);
    for (unsigned i=0;i<BLOCKS;i+=8) memcpy(local+0x10000+i,(uint8_t[]){0,0xf8,0,0,0,0,0,0},8);
    NVNtextureBuilder tb;
    NVNtexture textures[2],target;
    API(nvnTextureBuilderSetDevice)(&tb,device);API(nvnTextureBuilderSetDefaults)(&tb);
    API(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_2D);API(nvnTextureBuilderSetSize2D)(&tb,W,H);
    API(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGB_DXT1);
    assert(API(nvnTextureBuilderGetStorageSize)(&tb)==BLOCKS);
    for (unsigned i=0;i<2;i++) {
        API(nvnTextureBuilderSetStorage)(&tb,&sparse,i*BLOCKS);assert(API(nvnTextureInitialize)(&textures[i],&tb));
    }
    API(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
    API(nvnTextureBuilderSetStorage)(&tb,&pool,0x20000);assert(API(nvnTextureInitialize)(&target,&tb));
    NVNtexturePool tp;NVNsamplerPool sp;NVNsamplerBuilder sb;
    assert(API(nvnTexturePoolInitialize)(&tp,&pool,0x4000,257));
    assert(API(nvnSamplerPoolInitialize)(&sp,&pool,0x9000,257));
    API(nvnTexturePoolRegisterTexture)(&tp,256,&textures[1],NULL);
    API(nvnSamplerBuilderSetDefaults)(&sb);API(nvnSamplerBuilderSetDevice)(&sb,device);
    API(nvnSamplerBuilderSetMinMagFilter)(&sb,NVN_MIN_FILTER_NEAREST,NVN_MAG_FILTER_NEAREST);
    API(nvnSamplerPoolRegisterSamplerBuilder)(&sp,256,&sb);
    NVNtextureHandle handle=API(nvnDeviceGetTextureHandle)(device,256,256);
    NVNtextureView view;API(nvnTextureViewSetDefaults)(&view);API(nvnTextureViewSetFormat)(&view,NVN_FORMAT_RGBA16UI);
    NVNcommandBuffer cb;assert(API(nvnCommandBufferInitialize)(&cb,device));
    API(nvnCommandBufferAddCommandMemory)(&cb,&pool,0,0x3000);
    NVNcopyRegion full={0,0,0,W,H,1};NVNdrawTextureRegion rect={0,0,W,H};
    for (unsigned round=0;round<2;round++) {
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSetTexturePool)(&cb,&tp);API(nvnCommandBufferSetSamplerPool)(&cb,&sp);
        API(nvnCommandBufferSetCopyRowStride)(&cb,0);API(nvnCommandBufferSetCopyImageStride)(&cb,0);
        NVNchannelMaskState mask;API(nvnChannelMaskStateSetDefaults)(&mask);API(nvnCommandBufferBindChannelMaskState)(&cb,&mask);
        for (unsigned phase=0;phase<2;phase++) {
            if (phase && !round) API(nvnCommandBufferCopyBufferToTexture)(&cb,address+0x10000,&textures[0],NULL,&full,0);
            if (phase && round) {
                const NVNtexture *writer[]={&textures[0]};const NVNtextureView *views[]={&view};
                API(nvnCommandBufferSetRenderTargets)(&cb,1,writer,views,NULL,NULL);
                API(nvnCommandBufferSetScissor)(&cb,0,0,W/4,H/4);
                API(nvnCommandBufferClearColorui)(&cb,0,(uint32_t[]){0x7e0,0,0,0},15);
            }
            const NVNtexture *targets[]={&target};
            API(nvnCommandBufferSetRenderTargets)(&cb,1,targets,NULL,NULL,NULL);
            API(nvnCommandBufferSetViewport)(&cb,0,0,W,H);API(nvnCommandBufferSetScissor)(&cb,0,0,W,H);
            API(nvnCommandBufferDrawTexture)(&cb,handle,&rect,&rect);
            API(nvnCommandBufferCopyTextureToBuffer)(&cb,&target,NULL,&full,address+0x60000+phase*PIXELS,0);
        }
        NVNcommandHandle commands=API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue,1,&commands);API(nvnQueueFinish)(queue);
        const tex_t *alias=(const tex_t *)&textures[1];
        uint8_t tiled[BLOCKS],linear[BLOCKS];
        for (unsigned phase=0;phase<2;phase++) {
            memset(tiled,0,sizeof tiled);
            uint16_t endpoint=phase ? (round ? 0x7e0 : 0xf800) : (round ? 0xf800 : 0x1f);
            for (unsigned i=0;i<PAGE;i+=8) memcpy(tiled+i,&endpoint,2);
            assert(!nvn_deswizzle(&alias->d,0,0,tiled,linear,W/4*8));
            for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
                unsigned block=(y/4*(W/4)+x/4)*8;
                uint16_t word;memcpy(&word,linear+block,2);
                uint8_t expected[]={word==0xf800 ? 255 : 0,word==0x7e0 ? 255 : 0,word==0x1f ? 255 : 0,255};
                const uint8_t *got=local+0x60000+phase*PIXELS+(y*W+x)*4;
                if (memcmp(got,expected,4)) die("sparse encoded write round%u phase%u pixel%u,%u got %u,%u,%u,%u",round,phase,x,y,got[0],got[1],got[2],got[3]);
            }
        }
        for (unsigned i=0;i<PAGE;i+=8) {
            uint16_t endpoint=round ? 0x7e0 : 0xf800;
            assert(!memcmp(physical+i,&endpoint,2));
            assert(!memcmp(physical+PAGE+i,(uint8_t[]){0x1f,0,0,0,0,0,0,0},8));
        }
    }
    API(nvnCommandBufferFinalize)(&cb);API(nvnTexturePoolFinalize)(&tp);API(nvnSamplerPoolFinalize)(&sp);
    API(nvnTextureFinalize)(&target);for (unsigned i=0;i<2;i++) API(nvnTextureFinalize)(&textures[i]);
    API(nvnMemoryPoolFinalize)(&sparse);API(nvnMemoryPoolFinalize)(&backing);API(nvnMemoryPoolFinalize)(&pool);
    assert(!munmap(physical,2*PAGE));
    puts("Public sparse BC writes preserved physical aliases, holes, untouched backing and earlier draws across transfer and encoder-clear updates");
}
#endif
