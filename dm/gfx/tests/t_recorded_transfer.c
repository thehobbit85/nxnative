// Exercise the public NVN bootstrap registration and generated entry thunks on Metal or Vulkan.
// Only the backend's submission boundary is called directly. The recorder owns the command bytes.
#include "../nvn/hle_nvn.c"

static void *(*bootstrap)(const char *);
#define API(name) ((NVNT_##name)bootstrap(#name))

static void load_bootstrap(void) {
    unsigned long size;
    const hle_force_ent *table = DM_SECTION_DATA(swforce, &size);
    for (size_t i = 0; i < size / sizeof *table; i++) {
        if (strcmp(table[i].name, "nvnBootstrapLoader")) continue;
        assert(!bootstrap);
        bootstrap = table[i].fn;
    }
    assert(bootstrap);
    NVNT_nvnDeviceGetProcAddress get_proc = API(nvnDeviceGetProcAddress);
    for (size_t i = 0; i < NVN_NFUNC; i++) {
        void *entry = nvn_counting() ? nvn_thunk[i] : nvn_thunk_fast[i];
        assert(bootstrap(nvn_names[i]) == entry);
        assert((void *)get_proc(NULL, nvn_names[i]) == entry);
    }
}

uint64_t now_ns(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
uint64_t guest_ns(void) { return now_ns(); }
module_t *module_of(const void *p) { (void)p; return NULL; }

void *front_metal_layer(void) { return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}

#ifdef __ANDROID__
static void check_sparse_alias(NVNmemoryPoolBuilder *builder) {
    size_t page = (size_t)getpagesize();
    NVNmemoryPool physical, sparse, private_pool;
    uint8_t *src = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    uint8_t *other = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(src != MAP_FAILED && other != MAP_FAILED);
    API(nvnMemoryPoolBuilderSetFlags)(builder, NVN_MEMORY_POOL_FLAGS_PHYSICAL);
    API(nvnMemoryPoolBuilderSetStorage)(builder, src, page);
    assert(API(nvnMemoryPoolInitialize)(&physical, builder));
    API(nvnMemoryPoolBuilderSetStorage)(builder, other, page);
    assert(API(nvnMemoryPoolInitialize)(&private_pool, builder));
    API(nvnMemoryPoolBuilderSetFlags)(builder, NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    API(nvnMemoryPoolBuilderSetStorage)(builder, NULL, page * 2);
    assert(API(nvnMemoryPoolInitialize)(&sparse, builder));
    map_req map = {.phys = &physical, .size = page};
    assert(API(nvnMemoryPoolMapVirtual)(&sparse, 1, (const NVNmappingRequest *)&map));
    uint8_t *alias = (uint8_t *)API(nvnMemoryPoolGetBufferAddress)(&sparse);
    src[13] = 42; assert(alias[13] == 42);
    alias[17] = 55; assert(src[17] == 55);
    map.phys = &private_pool;
    assert(!API(nvnMemoryPoolMapVirtual)(&sparse, 1, (const NVNmappingRequest *)&map));
    assert(vpt_get(((pool_t *)&sparse)->vp, 0)->phys == src);
    assert(alias[13] == 42);
    map.phys = NULL;
    assert(API(nvnMemoryPoolMapVirtual)(&sparse, 1, (const NVNmappingRequest *)&map));
    API(nvnMemoryPoolFinalize)(&sparse);
    API(nvnMemoryPoolFinalize)(&private_pool);
    API(nvnMemoryPoolFinalize)(&physical);
    assert(!munmap(src, page) && !munmap(other, page));
}
#endif

int main(void) {
    load_bootstrap();
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
#ifdef __ANDROID__
    check_sparse_alias(&pb);
#endif
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
    typedef void (*push_fn)(NVNcommandBuffer *, uint64_t, uint64_t, uint64_t, const void *);
    push_fn push = (push_fn)h_nvnBootstrapLoader("nvnCommandBufferPushGraphicsConstants");
    uint32_t word = 37;
    push(&cb, 0, DMSH_PUSH_SIZE - sizeof word, sizeof word, &word);
    size_t used = API(nvnCommandBufferGetCommandMemoryUsed)(&cb);
    push(&cb, 0, UINT64_MAX, sizeof word, &word);
    assert(API(nvnCommandBufferGetCommandMemoryUsed)(&cb) == used);
    // Compare the units and epoch of a recorded timestamp with the public device clock.
    const size_t report_offset = dst_offset + 4096;
    NVNcounterData *reports = (NVNcounterData *)(storage + report_offset);
    API(nvnCommandBufferReportCounter)(&cb, NVN_COUNTER_TYPE_TIMESTAMP_TOP, addr + report_offset);
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
    API(nvnCommandBufferReportCounter)(&cb, NVN_COUNTER_TYPE_TIMESTAMP, addr + report_offset + sizeof *reports);
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
    uint64_t before = API(nvnDeviceGetCurrentTimestampInNanoseconds)(&device);
    mtl_sub_begin();
    mtl_exec((const uint8_t *)(uintptr_t)handle);
    mtl_sub_commit();
    NVNsyncWaitResult waited = API(nvnSyncWait)(&sync, 5000000000ull);
    assert(waited == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED || waited == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    uint64_t after = API(nvnDeviceGetCurrentTimestampInNanoseconds)(&device);
    for (int i = 0; i < 2; ++i) {
        uint64_t ns = API(nvnDeviceGetTimestampInNanoseconds)(&device, &reports[i]);
        assert(ns + 2 >= before && ns <= after + 2);
        assert(reports[i].counter == (DM_GAME_RENDER_COND ? 0 : reports[i].timestamp));
    }
    assert(reports[1].timestamp >= reports[0].timestamp);
    for (size_t i = 0; i < sizeof expected; i++) {
        if (storage[dst_offset + i] != expected[i])
            die("recorded transfer at byte %zu: got %#x, expected %#x", i, storage[dst_offset + i], expected[i]);
    }
    NVNtexturePool descriptors;
    assert(API(nvnTexturePoolInitialize)(&descriptors, &pool, 0xc000, 257));
    API(nvnTexturePoolRegisterImage)(&descriptors, 256, &tex, &view);
    API(nvnTexturePoolRegisterImage)(&descriptors, 256, NULL, NULL);
    API(nvnTexturePoolRegisterTexture)(&descriptors, 256, &tex, NULL);
    API(nvnTexturePoolFinalize)(&descriptors);
    API(nvnSyncFinalize)(&sync);
    API(nvnCommandBufferFinalize)(&cb);
    API(nvnTextureFinalize)(&tex);
    API(nvnMemoryPoolFinalize)(&pool);
    API(nvnDeviceFinalize)(&device);
    puts("Public NVN recorder returned both layers, preserved guards and padding, and reported timestamps on the guest clock");
}
