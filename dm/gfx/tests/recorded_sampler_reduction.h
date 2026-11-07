// Average, minimum and maximum of the same four texels, at two mip levels.
static void check_sampler_reduction(NVNdevice *device, NVNqueue *queue, NVNformat format) {
    static _Alignas(16384) uint8_t bytes[0x10000];
    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);
    API(nvnMemoryPoolBuilderSetDevice)(&pb, device);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, bytes, sizeof bytes);
    assert(API(nvnMemoryPoolInitialize)(&pool, &pb));
    NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(&pool);
    NVNtexture src, dst;
    NVNtextureBuilder tb;
    API(nvnTextureBuilderSetDefaults)(&tb);
    API(nvnTextureBuilderSetDevice)(&tb, device);
    API(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    API(nvnTextureBuilderSetFormat)(&tb, format);
    API(nvnTextureBuilderSetSize2D)(&tb, 4, 4);
    API(nvnTextureBuilderSetLevels)(&tb, 2);
    API(nvnTextureBuilderSetStorage)(&tb, &pool, 0x2000);
    assert(API(nvnTextureInitialize)(&src, &tb));
    API(nvnTextureBuilderSetSize2D)(&tb, 3, 1);
    API(nvnTextureBuilderSetLevels)(&tb, 1);
    API(nvnTextureBuilderSetStorage)(&tb, &pool, 0x3000);
    assert(API(nvnTextureInitialize)(&dst, &tb));
    NVNtexturePool tp;
    NVNsamplerPool sp;
    assert(API(nvnTexturePoolInitialize)(&tp, &pool, 0x6000, 257));
    assert(API(nvnSamplerPoolInitialize)(&sp, &pool, 0xa000, 259));
    API(nvnTexturePoolRegisterTexture)(&tp, 256, &src, NULL);
    NVNtextureView mip;
    API(nvnTextureViewSetDefaults)(&mip);
    API(nvnTextureViewSetLevels)(&mip, 1, 1);
    NVNsamplerBuilder sb;
    API(nvnSamplerBuilderSetDefaults)(&sb);
    API(nvnSamplerBuilderSetDevice)(&sb, device);
    NVNcommandBuffer cb;
    assert(API(nvnCommandBufferInitialize)(&cb, device));
    API(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0, 0x2000);
    float base[16], lower[] = {9, 11, 13, 15};
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) base[4*y+x] = 1 + 2*(x%2) + 4*(y%2);
    for (int i = 0; i < 20; ++i) {
        float f = i < 16 ? base[i] : lower[i-16];
        uint8_t *pixel = bytes + 0x4000 + i*4;
        if (format == NVN_FORMAT_R32F) memcpy(pixel, &f, sizeof f);
        else memcpy(pixel, (uint8_t[]){f, 20-f, 2*f, 255}, 4);
    }
    int modes = 3;
#ifndef DM_VK_PUBLIC_BINDINGS
    extern int metal_has_sampler_reduction(void);
    if (!metal_has_sampler_reduction()) modes = 1;
#endif
    static const struct {
        NVNdrawTextureRegion region;
        NVNminFilter min_filter;
        NVNmagFilter mag_filter;
        float lod, expected[3], anisotropy;
    } cases[] = {
        {{0,0,2,2}, NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_LINEAR, 1, {4,1,7}},
        {{1,1,3,3}, NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST, NVN_MAG_FILTER_LINEAR, 1, {12,9,15}},
        {{1,1,2,2}, NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_LINEAR, 0, {7,7,7}},  // adjacent texels have zero weight
        {{1,1,3,3}, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, NVN_MAG_FILTER_LINEAR, .5f, {8,1,15}},
        {{-.5f,0,.5f,1}, NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_LINEAR, 0, {2,1,3}},  // repeat across the left edge
        {{1,1,3,3}, NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST, NVN_MAG_FILTER_NEAREST, 1, {15,15,15}},
        {{0,0,4,1}, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, NVN_MAG_FILTER_LINEAR, 0, {2,1,3},4},
        {{0,0,1,4}, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, NVN_MAG_FILTER_LINEAR, 0, {3,1,5},4},
    };
    API(nvnSamplerBuilderSetWrapMode)(&sb, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSetCopyRowStride)(&cb, 0);
        API(nvnCommandBufferSetCopyImageStride)(&cb, 0);
        API(nvnCommandBufferCopyBufferToTexture)(&cb, addr+0x4000, &src, NULL, &(NVNcopyRegion){0,0,0,4,4,1}, 0);
        API(nvnCommandBufferCopyBufferToTexture)(&cb, addr+0x4040, &src, &mip, &(NVNcopyRegion){0,0,0,2,2,1}, 0);
        API(nvnCommandBufferSetRenderTargets)(&cb, 1, (const NVNtexture *[]){&dst}, NULL, NULL, NULL);
        API(nvnCommandBufferSetScissor)(&cb, 0, 0, 3, 1);
        API(nvnCommandBufferSetTexturePool)(&cb, &tp);
        API(nvnCommandBufferSetSamplerPool)(&cb, &sp);
        NVNchannelMaskState mask;
        API(nvnChannelMaskStateSetDefaults)(&mask);
        API(nvnCommandBufferBindChannelMaskState)(&cb, &mask);
        for (int mode = 0; mode < modes; ++mode) {
            API(nvnSamplerBuilderSetMinMagFilter)(&sb, cases[i].min_filter, cases[i].mag_filter);
            API(nvnSamplerBuilderSetLodClamp)(&sb, cases[i].lod, cases[i].lod);
            API(nvnSamplerBuilderSetReductionFilter)(&sb, mode);
            API(nvnSamplerBuilderSetMaxAnisotropy)(&sb, cases[i].anisotropy ? cases[i].anisotropy : 1);
            API(nvnSamplerPoolRegisterSamplerBuilder)(&sp, 256+mode, &sb);
            API(nvnCommandBufferDrawTexture)(&cb, API(nvnDeviceGetTextureHandle)(device,256,256+mode),
                                              &(NVNdrawTextureRegion){mode,0,mode+1,1}, &cases[i].region);
        }
        API(nvnCommandBufferCopyTextureToBuffer)(&cb, &dst, NULL, &(NVNcopyRegion){0,0,0,3,1,1}, addr+0x4080, 0);
        NVNcommandHandle list = API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue, 1, &list);
        API(nvnQueueFinish)(queue);
        float actual[3];
        memcpy(actual, bytes+0x4080, sizeof actual);
        for (int mode = 0; mode < modes; ++mode) {
            float expected = cases[i].expected[mode];
            if (format == NVN_FORMAT_R32F) {
                float error=actual[mode]-expected,tolerance=!mode && cases[i].anisotropy>1 ? .00001f : 0;
                if (!(error>=-tolerance && error<=tolerance))
                    die("R32F sampler case %zu mode %d: got %.9g expected %.9g", i, mode, actual[mode], expected);
            } else {
                // Green falls as red rises, so their extrema come from different texels.
                uint8_t rgba[] = {expected, 20-cases[i].expected[mode ? 3-mode : 0], 2*expected, 255};
                const uint8_t *out = bytes+0x4080+4*mode;
                for (int c = 0; c < 4; ++c) if (out[c] != rgba[c])
                    die("RGBA8 sampler case %zu mode %d channel %d: got %u expected %u", i, mode, c, out[c], rgba[c]);
            }
        }
    }
    API(nvnCommandBufferFinalize)(&cb);
    API(nvnSamplerPoolFinalize)(&sp);
    API(nvnTexturePoolFinalize)(&tp);
    API(nvnTextureFinalize)(&dst);
    API(nvnTextureFinalize)(&src);
    API(nvnMemoryPoolFinalize)(&pool);
    puts(modes == 3 ? "Public NVN sampler reduction: average/min/max across mip levels, repeat edges, exact centers, anisotropic and nearest filtering"
                    : "Public NVN sampler: average across eight footprints; minimum and maximum require Apple GPU family 10");
}
