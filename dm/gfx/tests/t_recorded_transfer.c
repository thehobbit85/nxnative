// Exercise the public NVN entry implementations on the Mac, where the recorder currently builds.
// Only the backend's submission boundary is called directly. The recorder owns the command bytes.
#include "../nvn/hle_nvn.c"

#define API(name) ((NVNT_##name)h_nvnBootstrapLoader(#name))

uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
uint64_t guest_ns(void) { return now_ns(); }
module_t *module_of(const void *p) { (void)p; return NULL; }

void *front_metal_layer(void) { return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}

int main(void) {
    enum { pool_size = 0x10000, cmd_size = 0x4000, tex_offset = 0x4000, dst_offset = 0x8000,
           row_pitch = 24, image_pitch = 80, dst_bytes = 176, guard = 8 };
    static _Alignas(16384) uint8_t storage[pool_size];
    NVNdevice device;
    NVNdeviceBuilder db;
    API(nvnDeviceBuilderSetDefaults)(&db);
    assert(API(nvnDeviceInitialize)(&device, &db));

    NVNmemoryPool pool;
    NVNmemoryPoolBuilder pb;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);
    API(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, storage, sizeof storage);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    assert(API(nvnMemoryPoolInitialize)(&pool, &pb));
    NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(&pool);

    NVNtexture tex;
    NVNtextureBuilder tb;
    API(nvnTextureBuilderSetDevice)(&tb, &device);
    API(nvnTextureBuilderSetDefaults)(&tb);
    API(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D_ARRAY);
    API(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    API(nvnTextureBuilderSetSize3D)(&tb, 8, 8, 3);
    assert(tex_offset % API(nvnTextureBuilderGetStorageAlignment)(&tb) == 0);
    assert(tex_offset + API(nvnTextureBuilderGetStorageSize)(&tb) <= dst_offset);
    API(nvnTextureBuilderSetStorage)(&tb, &pool, tex_offset);
    assert(API(nvnTextureInitialize)(&tex, &tb));

    NVNcommandBuffer cb;
    assert(API(nvnCommandBufferInitialize)(&cb, &device));
    API(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0, cmd_size);
    API(nvnCommandBufferBeginRecording)(&cb);
    NVNcopyRegion all = {0, 0, 0, 8, 8, 3}, middle = {0, 0, 1, 8, 8, 1};
    float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
    API(nvnCommandBufferClearTexture)(&cb, &tex, NULL, &all, blue, 15);
    API(nvnCommandBufferClearTexture)(&cb, &tex, NULL, &middle, red, 15);
    API(nvnCommandBufferSetCopyRowStride)(&cb, row_pitch);
    API(nvnCommandBufferSetCopyImageStride)(&cb, image_pitch);
    NVNtextureView view;
    API(nvnTextureViewSetDefaults)(&view);
    API(nvnTextureViewSetLayers)(&view, 1, 2);
    NVNcopyRegion sub = {2, 1, 0, 3, 3, 2};
    API(nvnCommandBufferCopyTextureToBuffer)(&cb, &tex, &view, &sub, addr + dst_offset + guard, 0);
    NVNsync sync;
    assert(API(nvnSyncInitialize)(&sync, &device));
    API(nvnCommandBufferFenceSync)(&cb, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle handle = API(nvnCommandBufferEndRecording)(&cb);
    assert(handle == (uintptr_t)storage);

    uint8_t expected[dst_bytes];
    memset(storage + dst_offset, 0xcc, dst_bytes);
    memset(expected, 0xcc, sizeof expected);
    for (int layer = 0; layer < 2; layer++) {
        const uint8_t color[4] = {layer == 0 ? 255 : 0, 0, layer == 1 ? 255 : 0, 255};
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++)
                memcpy(expected + guard + layer * image_pitch + y * row_pitch + 4 * x, color, sizeof color);
    }

    assert(API(nvnSyncWait)(&sync, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
    mtl_sub_begin();
    mtl_exec((const uint8_t *)(uintptr_t)handle);
    mtl_sub_commit();
    NVNsyncWaitResult waited = API(nvnSyncWait)(&sync, 5000000000ull);
    assert(waited == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED || waited == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    for (size_t i = 0; i < sizeof expected; i++) {
        if (storage[dst_offset + i] != expected[i])
            die("recorded transfer at byte %zu: got %#x, expected %#x", i, storage[dst_offset + i], expected[i]);
    }
    API(nvnSyncFinalize)(&sync);
    API(nvnCommandBufferFinalize)(&cb);
    API(nvnTextureFinalize)(&tex);
    API(nvnMemoryPoolFinalize)(&pool);
    API(nvnDeviceFinalize)(&device);
    puts("Public NVN recorder returned both colored layers and preserved every guard and padding byte");
}
