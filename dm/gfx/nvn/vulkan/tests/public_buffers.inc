// Exercise both public UBO record encodings with the same commands. The full
// encoding needs a range larger than UINT32_MAX; anonymous pages are demand
// allocated, and the shaders only read the first vector of each range.
static void check_buffer_bindings(NVNdevice *device, NVNqueue *queue) {
    const size_t four_gib = UINT64_C(1) << 32;
    const size_t large = four_gib + 16, size = four_gib + 0x100000;
    uint8_t *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);
    API(nvnMemoryPoolBuilderSetDevice)(&pb, device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, memory, size);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    assert(API(nvnMemoryPoolInitialize)(&pool, &pb));
    NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(&pool);
    assert(addr > UINT32_MAX);

    uint8_t control[0x880] = { [0x704] = 1 };
    NVNshaderData shaders[3];
    for (int i = 0; i < 3; ++i) {
        memory[0x9000 + i] = 4 + i;
        shaders[i] = (NVNshaderData){addr + 0x9000 + i, control};
    }
    NVNprogram graphics, compute;
    assert(API(nvnProgramInitialize)(&graphics, device));
    assert(API(nvnProgramInitialize)(&compute, device));
    assert(API(nvnProgramSetShaders)(&graphics, 2, shaders));
    assert(API(nvnProgramSetShaders)(&compute, 1, shaders + 2));

    NVNtexture target;
    NVNtextureBuilder tb;
    API(nvnTextureBuilderSetDefaults)(&tb);
    API(nvnTextureBuilderSetDevice)(&tb, device);
    API(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    API(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA32UI);
    API(nvnTextureBuilderSetSize2D)(&tb, 1, 1);
    assert(API(nvnTextureBuilderGetStorageSize)(&tb) <= 0x4000);
    API(nvnTextureBuilderSetStorage)(&tb, &pool, 0x4000);
    assert(API(nvnTextureInitialize)(&target, &tb));
    const NVNtexture *color = &target;
    NVNcopyRegion pixel = {0, 0, 0, 1, 1, 1};
    NVNcommandBuffer cb;
    assert(API(nvnCommandBufferInitialize)(&cb, device));
    API(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0, 0x4000);

    for (int full = 0; full <= 1; ++full) {
        size_t start = 0x10000 + full * 0x50000;
        size_t constants[] = {start, start + 0x10000, start + 0x20000};
        size_t counters = start + 0x30000, pixels = counters + 0x100;
        const NVNshaderStage stage[] = {NVN_SHADER_STAGE_VERTEX, NVN_SHADER_STAGE_FRAGMENT, NVN_SHADER_STAGE_COMPUTE};
        const uint32_t values[][4] = {{11,12,13,14}, {31,32,33,34}, {41,42,43,44}};
        for (int i = 0; i < 3; ++i) memcpy(memory + constants[i], values[i], sizeof values[i]);
        memset(memory + counters - 4, 0xcc, 64);
        memset(memory + counters, 0, 24);
        memset(memory + pixels, 0xcc, 32);
        NVNsync fence;
        assert(API(nvnSyncInitialize)(&fence, device));
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSetRenderTargets)(&cb, 1, &color, NULL, NULL, NULL);
        API(nvnCommandBufferSetViewport)(&cb, 0, 0, 1, 1);
        API(nvnCommandBufferSetScissor)(&cb, 0, 0, 1, 1);
        API(nvnCommandBufferBindProgram)(&cb, &graphics, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
        API(nvnCommandBufferBindProgram)(&cb, &compute, NVN_SHADER_STAGE_COMPUTE_BIT);
        for (int i = 0; i < 3; ++i) {
            size_t before = API(nvnCommandBufferGetCommandMemoryUsed)(&cb);
            API(nvnCommandBufferBindUniformBuffer)(&cb, stage[i], 7, addr + constants[i], full ? large : 16);
            assert(API(nvnCommandBufferGetCommandMemoryUsed)(&cb) - before == (full ? 32 : 24));
            API(nvnCommandBufferBindStorageBuffer)(&cb, stage[i], 3, addr + counters + 4 * i, 16);
        }
        // Compute modifies just the first word of the fragment uniform. The
        // inline update below writes its neighbor; vertex constants stay live.
        API(nvnCommandBufferBindStorageBuffer)(&cb, NVN_SHADER_STAGE_COMPUTE, 0, addr + constants[1], 4);
        API(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        API(nvnCommandBufferCopyTextureToBuffer)(&cb, color, NULL, &pixel, addr + pixels, 0);
        int barrier = NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT;
        API(nvnCommandBufferBarrier)(&cb, barrier);
        API(nvnCommandBufferDispatchCompute)(&cb, 1, 1, 1);
        API(nvnCommandBufferBarrier)(&cb, barrier);
        uint32_t update = 87;
        API(nvnCommandBufferUpdateUniformBuffer)(&cb, addr + constants[1], 16, 4, sizeof update, &update);
        API(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        API(nvnCommandBufferCopyTextureToBuffer)(&cb, color, NULL, &pixel, addr + pixels + 16, 0);
        API(nvnCommandBufferFenceSync)(&cb, &fence, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
        NVNcommandHandle commands = API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue, 1, &commands);
        API(nvnQueueFlush)(queue);
        NVNsyncWaitResult waited = API(nvnSyncWait)(&fence, 5000000000ull);
        assert(waited == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED || waited == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
        const uint32_t expected_pixels[] = {11,12,31,32,11,12,43,87};
        for (int i = 0; i < 8; ++i) {
            uint32_t got = ((uint32_t *)(memory + pixels))[i];
            if (got != expected_pixels[i]) die("public UBO full=%d pixel %d component %d: %u expected %u",
                                                full, i / 4, i % 4, got, expected_pixels[i]);
        }
        const uint32_t expected_words[] = {0xcccccccc,22,98,160,42,0,32,0xcccccccc};
        for (int i = 0; i < 8; ++i) {
            uint32_t got = ((uint32_t *)(memory + counters - 4))[i];
            if (got != expected_words[i]) die("public SSBO full=%d word %d: %u expected %u",
                                               full, i - 1, got, expected_words[i]);
        }
        API(nvnSyncFinalize)(&fence);
    }
    API(nvnQueueFinish)(queue);
    API(nvnCommandBufferFinalize)(&cb);
    API(nvnTextureFinalize)(&target);
    API(nvnProgramFinalize)(&compute);
    API(nvnProgramFinalize)(&graphics);
    API(nvnMemoryPoolFinalize)(&pool);
    assert(!munmap(memory, size));
    puts("Public compact/full UBO and storage ranges: VS/FS/CS share slot numbers, ordered pixels and atomics match after fence");
}
