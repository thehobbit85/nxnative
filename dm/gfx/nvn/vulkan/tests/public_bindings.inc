// Included by the public NVN recorder fixture when linked against Vulkan.
// The package contains the test shaders under the actual keys of these
// synthetic guest stages (zeroed control section and one code byte per stage).
static void check_deferred_cubes(NVNdevice *device, NVNqueue *queue, NVNcommandBuffer *cb, NVNmemoryPool *pool,
                                 NVNtexturePool tp[2], NVNsamplerPool sp[2], const NVNprogram *program) {
    enum { data = 0x46000, result = data + 128 };
    uint8_t *memory = API(nvnMemoryPoolMap)(pool);
    NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(pool);
    NVNtexture images[5];
    NVNtextureBuilder tb;
    API(nvnTextureBuilderSetDefaults)(&tb);
    API(nvnTextureBuilderSetDevice)(&tb, device);
    API(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    NVNcopyRegion faces = {0, 0, 0, 1, 1, 6}, row = {0, 0, 0, 6, 1, 1};
    API(nvnCommandBufferBeginRecording)(cb);
    API(nvnCommandBufferSetCopyRowStride)(cb, 0);
    API(nvnCommandBufferSetCopyImageStride)(cb, 0);
    for (int t = 0; t < 5; ++t) {
        API(nvnTextureBuilderSetTarget)(&tb, t < 4 ? NVN_TEXTURE_TARGET_CUBEMAP : NVN_TEXTURE_TARGET_2D);
        API(nvnTextureBuilderSetSize3D)(&tb, t < 4 ? 1 : 6, 1, t < 4 ? 6 : 1);
        assert(API(nvnTextureBuilderGetStorageSize)(&tb) <= 0x4000);
        API(nvnTextureBuilderSetStorage)(&tb, pool, 0x32000 + 0x4000 * t);
        assert(API(nvnTextureInitialize)(&images[t], &tb));
        if (t == 4) break;
        API(nvnTexturePoolRegisterTexture)(&tp[t / 2], 256 + t % 2, &images[t], NULL);
        for (int f = 0; f < 6; ++f) {
            memory[data + 24 * t + 4 * f] = 1 + 48 * t + 7 * f;
            memory[data + 24 * t + 4 * f + 3] = 255;
        }
        API(nvnCommandBufferCopyBufferToTexture)(cb, addr + data + 24 * t, &images[t], NULL, &faces, 0);
    }
    NVNcommandHandle upload = API(nvnCommandBufferEndRecording)(cb);
    API(nvnQueueSubmitCommands)(queue, 1, &upload);
    API(nvnQueueFinish)(queue);

    typedef void (*deferred_fn)(NVNcommandBuffer *, NVNshaderStage, int, int, const NVNtextureHandle *);
    deferred_fn bind = (deferred_fn)bootstrap("nvnCommandBufferBindTexturesDeferred");
    assert(bind);
    const NVNtexture *target = &images[4];
    for (int p = 0; p < 2; ++p) {
        NVNtextureHandle handles[2] = {0};
        API(nvnCommandBufferBeginRecording)(cb);
        API(nvnCommandBufferSetRenderTargets)(cb, 1, &target, NULL, NULL, NULL);
        API(nvnCommandBufferSetViewport)(cb, 0, 0, 6, 1);
        API(nvnCommandBufferSetScissor)(cb, 0, 0, 6, 1);
        API(nvnCommandBufferBindProgram)(cb, program, 3);
        API(nvnCommandBufferSetTexturePool)(cb, &tp[p]);
        API(nvnCommandBufferSetSamplerPool)(cb, &sp[p]);
        bind(cb, NVN_SHADER_STAGE_FRAGMENT, 3, 2, handles);
        API(nvnCommandBufferDrawArrays)(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        API(nvnCommandBufferCopyTextureToBuffer)(cb, target, NULL, &row, addr + result, 0);
        NVNcommandHandle commands = API(nvnCommandBufferEndRecording)(cb);
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < 2; ++i)
                handles[i] = API(nvnDeviceGetTextureHandle)(device, 256 + (i ^ pass), 256);
            API(nvnQueueSubmitCommands)(queue, 1, &commands);
            API(nvnQueueFinish)(queue);
            for (int f = 0; f < 6; ++f) {
                int base = 1 + 96 * p + 7 * f;
                uint8_t expected[] = {base + 48 * pass, base + 48 * (1 - pass), 0, 255};
                const uint8_t *pixel = memory + result + 4 * f;
                if (memcmp(pixel, expected, 4))
                    die("deferred pool %d pass %d face %d returned %u/%u, expected %u/%u", p, pass, f,
                        pixel[0], pixel[1], expected[0], expected[1]);
            }
        }
    }
    for (int t = 0; t < 5; ++t) API(nvnTextureFinalize)(&images[t]);
    puts("Public deferred cube binds: six faces from both slots, refreshed after recording in both pools");
}

static void check_texture_bindings(NVNdevice *device, NVNqueue *queue) {
    enum { pool_size = 0x50000, source_offset = 0x30000, result_offset = 0x31000 };
    static _Alignas(16384) uint8_t memory[pool_size];
    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);
    API(nvnMemoryPoolBuilderSetDevice)(&pb, device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, memory, sizeof memory);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    assert(API(nvnMemoryPoolInitialize)(&pool, &pb));
    NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(&pool);

    uint8_t control[0x880] = { [0x704] = 1 };
    NVNprogram programs[3];
    for (int i = 0; i < 4; ++i) memory[source_offset + 64 + i] = i;
    for (int i = 0; i < 3; ++i) {
        NVNshaderData stages[] = {{addr + source_offset + 64, control}, {addr + source_offset + 65 + i, control}};
        assert(API(nvnProgramInitialize)(&programs[i], device));
        assert(API(nvnProgramSetShaders)(&programs[i], 2, stages));
    }

    NVNtexture textures[5];
    NVNtextureBuilder tb;
    API(nvnTextureBuilderSetDefaults)(&tb);
    API(nvnTextureBuilderSetDevice)(&tb, device);
    API(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    API(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    for (int t = 0; t < 5; ++t) {
        API(nvnTextureBuilderSetSize2D)(&tb, t == 4 ? 4 : 2, 1);
        assert(API(nvnTextureBuilderGetStorageSize)(&tb) <= 0x1000);
        API(nvnTextureBuilderSetStorage)(&tb, &pool, 0x18000 + 0x1000 * t);
        assert(API(nvnTextureInitialize)(&textures[t], &tb));
    }

    NVNtexturePool tp[2];
    NVNsamplerPool sp[2];
    NVNsamplerBuilder sb;
    API(nvnSamplerBuilderSetDefaults)(&sb);
    API(nvnSamplerBuilderSetDevice)(&sb, device);
    for (int p = 0; p < 2; ++p) {
        assert(API(nvnTexturePoolInitialize)(&tp[p], &pool, 0x20000 + p * 0x4000, 258));
        assert(API(nvnSamplerPoolInitialize)(&sp[p], &pool, 0x28000 + p * 0x4000, 258));
        for (int i = 0; i < 2; ++i) {
            API(nvnTexturePoolRegisterTexture)(&tp[p], 256 + i, &textures[2 * p + i], NULL);
            int linear = p ^ i;
            NVNwrapMode wrap = linear ? NVN_WRAP_MODE_CLAMP_TO_EDGE : NVN_WRAP_MODE_REPEAT;
            API(nvnSamplerBuilderSetMinMagFilter)(&sb, linear ? NVN_MIN_FILTER_LINEAR : NVN_MIN_FILTER_NEAREST,
                                                 linear ? NVN_MAG_FILTER_LINEAR : NVN_MAG_FILTER_NEAREST);
            API(nvnSamplerBuilderSetWrapMode)(&sb, wrap, wrap, wrap);
            API(nvnSamplerPoolRegisterSamplerBuilder)(&sp[p], 256 + i, &sb);
        }
    }

    NVNcommandBuffer cb;
    assert(API(nvnCommandBufferInitialize)(&cb, device));
    API(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0, 0x18000);
    API(nvnCommandBufferBeginRecording)(&cb);
    NVNcopyRegion pair = {0, 0, 0, 2, 1, 1}, row = {0, 0, 0, 4, 1, 1};
    for (int t = 0; t < 4; ++t) {
        uint8_t *src = memory + source_offset + 8 * t;
        src[0] = 32 * t; src[4] = src[0] + 96;
        src[3] = src[7] = 255;
        API(nvnCommandBufferCopyBufferToTexture)(&cb, addr + source_offset + 8 * t, &textures[t], NULL, &pair, 0);
    }
    NVNcommandHandle upload = API(nvnCommandBufferEndRecording)(&cb);
    API(nvnQueueSubmitCommands)(queue, 1, &upload);
    API(nvnQueueFinish)(queue);

    const NVNtexture *target = &textures[4];
    for (int separate = 0; separate < 2; ++separate) for (int p = 0; p < 2; ++p) {
        size_t at = 0;
        NVNcommandHandle commands = 0;
        for (int pass = 0; pass < 4; ++pass) {
            int image = pass & 1, sampler = pass >> 1;
            // Single-handle recordings are patched and resubmitted; separate
            // texture/sampler binds each snapshot the supplied descriptor.
            if (separate || !pass) {
                API(nvnCommandBufferBeginRecording)(&cb);
                API(nvnCommandBufferSetRenderTargets)(&cb, 1, &target, NULL, NULL, NULL);
                API(nvnCommandBufferBindProgram)(&cb, &programs[separate], NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
                API(nvnCommandBufferSetViewport)(&cb, 0, 0, 4, 1);
                API(nvnCommandBufferSetScissor)(&cb, 0, 0, 4, 1);
                API(nvnCommandBufferSetTexturePool)(&cb, &tp[p]);
                API(nvnCommandBufferSetSamplerPool)(&cb, &sp[p]);
                if (separate) {
                    NVNseparateTextureHandle tex = API(nvnDeviceGetSeparateTextureHandle)(device, 256 + image);
                    NVNseparateSamplerHandle samp = API(nvnDeviceGetSeparateSamplerHandle)(device, 256 + sampler);
                    API(nvnCommandBufferBindSeparateTexture)(&cb, NVN_SHADER_STAGE_FRAGMENT, 3, tex);
                    API(nvnCommandBufferBindSeparateSampler)(&cb, NVN_SHADER_STAGE_FRAGMENT, 7, samp);
                } else {
                    at = API(nvnCommandBufferGetCommandMemoryUsed)(&cb);
                    API(nvnCommandBufferBindTexture)(&cb, NVN_SHADER_STAGE_FRAGMENT, 3, 0);
                    assert(API(nvnCommandBufferGetCommandMemoryUsed)(&cb) - at == (p ? 32 : 24));
                }
                API(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
                API(nvnCommandBufferCopyTextureToBuffer)(&cb, target, NULL, &row, addr + result_offset, 0);
                API(nvnCommandBufferReportValue)(&cb, 1234, addr + result_offset + 16);
                commands = API(nvnCommandBufferEndRecording)(&cb);
            }
            if (!separate) {
                uint32_t word = API(nvnDeviceGetTextureHandle)(device, 256 + image, 256 + sampler);
                memcpy(memory + at + 16, &word, sizeof word);
            }
            memset(memory + result_offset, 0xcc, 20);
            API(nvnQueueSubmitCommands)(queue, 1, &commands);
            API(nvnQueueFinish)(queue);
            uint8_t left = 32 * (2 * p + image);
            for (int x = 0; x < 4; ++x) {
                uint8_t red = left + ((sampler ^ p) ? (x ? 96 : 24) : 96 * (x & 1));
                const uint8_t expected[4] = {red, 0, 0, 255};
                uint8_t *got = memory + result_offset + 4 * x;
                if (memcmp(expected, got, 4)) die("pool %d image %d sampler %d pixel %d: %u %u %u %u, expected red %u",
                                                 p, image, sampler, x, got[0], got[1], got[2], got[3], red);
            }
            uint32_t following;
            memcpy(&following, memory + result_offset + 16, sizeof following);
            assert(following == 1234);
        }
    }
    check_deferred_cubes(device, queue, &cb, &pool, tp, sp, &programs[2]);
    API(nvnCommandBufferFinalize)(&cb);
    for (int p = 0; p < 2; ++p) {
        API(nvnTexturePoolFinalize)(&tp[p]);
        API(nvnSamplerPoolFinalize)(&sp[p]);
    }
    for (int t = 0; t < 5; ++t) API(nvnTextureFinalize)(&textures[t]);
    for (int i = 0; i < 3; ++i) API(nvnProgramFinalize)(&programs[i]);
    API(nvnMemoryPoolFinalize)(&pool);
    puts("Public NVN texture binds: compact and saved pool bases, live handle patch, combined and separate nearest/repeat and linear/clamp");
}
