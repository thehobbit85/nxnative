#include "../tex/nvn_deswizzle.h"

// Re-submit the same public sampled draws after changing a virtual pool.
// Two textures share a physical page. The other pages are remapped and
// removed independently; unmapped texels must sample as transparent black.
static void check_sparse_textures(NVNdevice *device, NVNqueue *queue) {
    enum { page=0x4000, w=128, h=64, texture_bytes=w*h*4 };
    static _Alignas(16384) uint8_t local[0x30000];
    uint8_t *phys=mmap(NULL,3*page,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANON,-1,0);
    assert(phys!=MAP_FAILED);
    for (int i=0;i<3;++i) memset(phys+i*page,0x22+0x33*i,page);
    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool, physical, sparse;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb,device);
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_PHYSICAL|NVN_MEMORY_POOL_FLAGS_CPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,phys,3*page);
    assert(API(nvnMemoryPoolInitialize)(&physical,&pb));
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,NULL,4*page);
    assert(API(nvnMemoryPoolInitialize)(&sparse,&pb));
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,local,sizeof local);
    assert(API(nvnMemoryPoolInitialize)(&pool,&pb));
    NVNbufferAddress addr=API(nvnMemoryPoolGetBufferAddress)(&pool);
    NVNtexture tx[2], target;
    NVNtextureBuilder tb;
    NVNpackagedTextureLayout layout={0};
    API(nvnTextureBuilderSetDefaults)(&tb); API(nvnTextureBuilderSetDevice)(&tb,device);
    API(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8); API(nvnTextureBuilderSetSize2D)(&tb,w,h);
    API(nvnTextureBuilderSetStorage)(&tb,&pool,0x10000);
    assert(API(nvnTextureBuilderGetStorageSize)(&tb)==texture_bytes);
    assert(API(nvnTextureInitialize)(&target,&tb));
    API(nvnTextureBuilderSetPackagedTextureLayout)(&tb,&layout);
    assert(API(nvnTextureBuilderGetStorageSize)(&tb)==texture_bytes);
    for (int i=0;i<2;++i) {
        if (i==1) API(nvnTextureBuilderSetPackagedTextureLayout)(&tb,NULL); // ordinary sampled texture
        API(nvnTextureBuilderSetStorage)(&tb,&sparse,i*texture_bytes);
        assert(API(nvnTextureInitialize)(&tx[i],&tb));
    }
    NVNtexturePool tp;
    NVNsamplerPool sp;
    assert(API(nvnTexturePoolInitialize)(&tp,&pool,0x4000,258));
    assert(API(nvnSamplerPoolInitialize)(&sp,&pool,0x9000,257));
    for (int i=0;i<2;++i) API(nvnTexturePoolRegisterTexture)(&tp,256+i,&tx[i],NULL);
    NVNsamplerBuilder sb;
    API(nvnSamplerBuilderSetDefaults)(&sb); API(nvnSamplerBuilderSetDevice)(&sb,device);
    API(nvnSamplerBuilderSetMinMagFilter)(&sb,NVN_MIN_FILTER_NEAREST,NVN_MAG_FILTER_NEAREST);
    API(nvnSamplerPoolRegisterSamplerBuilder)(&sp,256,&sb);
    NVNcommandBuffer cb;
    assert(API(nvnCommandBufferInitialize)(&cb,device));
    API(nvnCommandBufferAddCommandMemory)(&cb,&pool,0,0x3000); API(nvnCommandBufferBeginRecording)(&cb);
    const NVNtexture *targets[]={&target};
    API(nvnCommandBufferSetRenderTargets)(&cb,1,targets,NULL,NULL,NULL);
    API(nvnCommandBufferSetViewport)(&cb,0,0,w,h); API(nvnCommandBufferSetScissor)(&cb,0,0,w,h);
    API(nvnCommandBufferSetTexturePool)(&cb,&tp); API(nvnCommandBufferSetSamplerPool)(&cb,&sp);
    NVNchannelMaskState mask; API(nvnChannelMaskStateSetDefaults)(&mask); API(nvnCommandBufferBindChannelMaskState)(&cb,&mask);
    API(nvnCommandBufferSetCopyRowStride)(&cb,0); API(nvnCommandBufferSetCopyImageStride)(&cb,0);
    NVNcopyRegion all={0,0,0,w,h,1}; NVNdrawTextureRegion rect={0,0,w,h};
    for (int i=0;i<2;++i) {
        NVNtextureHandle handle=API(nvnDeviceGetTextureHandle)(device,256+i,256);
        API(nvnCommandBufferDrawTexture)(&cb,handle,&rect,&rect);
        API(nvnCommandBufferCopyTextureToBuffer)(&cb,&target,NULL,&all,addr+0x1a000+i*texture_bytes,0);
    }
    NVNcommandHandle draws=API(nvnCommandBufferEndRecording)(&cb);
    const int backing[][4]={{0,1,0,2},{0,2,0,-1},{0,2,0,-1},{-1,2,0,0},{-1,-1,-1,-1}};
    for (size_t round=0;round<sizeof backing/sizeof *backing;++round) {
        for (int i=0;i<4;++i) {
            int src=backing[round][i];
            if (round && src==backing[round-1][i]) continue;
            map_req m={.phys=src<0 ? NULL : &physical,.poff=src<0 ? 0 : src*page,.voff=i*page,.size=page};
            assert(API(nvnMemoryPoolMapVirtual)(&sparse,1,(const NVNmappingRequest *)&m));
        }
        if (round==2) { dm_guest_write_prepare(phys,page); memset(phys,0xcd,page); }
        API(nvnQueueSubmitCommands)(queue,1,&draws); API(nvnQueueFinish)(queue);
        for (int i=0;i<2;++i) {
            uint8_t tiled[texture_bytes], expected[texture_bytes];
            for (int tile=0;tile<2;++tile) {
                int src=backing[round][2*i+tile];
                uint8_t value=src<0 ? 0 : src==0 && round>=2 ? 0xcd : 0x22+0x33*src;
                memset(tiled+tile*page,value,page);
            }
            const tex_t *texture=(const tex_t *)&tx[i];
            assert(!nvn_deswizzle(&texture->d,0,0,tiled,expected,w*4));
            for (int k=0;k<texture_bytes;++k) {
                uint8_t got=local[0x1a000+i*texture_bytes+k];
                if (got!=expected[k]) die("sparse round %zu texture %d byte %d: %#x expected %#x",
                                         round,i,k,got,expected[k]);
            }
        }
    }
    API(nvnCommandBufferFinalize)(&cb); API(nvnTexturePoolFinalize)(&tp); API(nvnSamplerPoolFinalize)(&sp);
    API(nvnTextureFinalize)(&target); for (int i=0;i<2;++i) API(nvnTextureFinalize)(&tx[i]);
    API(nvnMemoryPoolFinalize)(&sparse); API(nvnMemoryPoolFinalize)(&physical); API(nvnMemoryPoolFinalize)(&pool);
    assert(!munmap(phys,3*page));
    puts("Public NVN sparse textures: sampled shared physical data, remaps, CPU changes and zero-filled holes");
}
