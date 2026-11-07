// Exercise the public NVN bootstrap registration and generated entry thunks on Metal or Vulkan.
// Submit through the public queue as well: this exercises backend preparation,
// queue-state selection and capture hooks in the same order as the application.
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

typedef struct {
    NVNmemoryPool *pool;
    int calls;
} refill_context;

static void refill(NVNcommandBuffer *cb, NVNcommandBufferMemoryEvent reason, size_t need, void *user) {
    refill_context *ctx = user;
    assert(reason == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY && need <= 256);
    assert(++ctx->calls == 1);
    API(nvnCommandBufferAddCommandMemory)(cb, ctx->pool, 0x3000, 256);
}

static NVNcommandHandle record_value(NVNcommandBuffer *cb, NVNbufferAddress dst, uint32_t value) {
    API(nvnCommandBufferBeginRecording)(cb);
    API(nvnCommandBufferReportValue)(cb, value, dst);
    return API(nvnCommandBufferEndRecording)(cb);
}

static void check_event_lists(NVNdevice *device, NVNqueue *queue, NVNmemoryPool *pool, uint8_t *storage) {
    enum { results = 0xb000, segment = 0x2000 };
    const uint32_t guard = 0xa5a5a5a5;
    uint32_t *words = (uint32_t *)(storage + results);
    NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(pool) + results;
    for (int i = 0; i < 8; ++i) words[i] = guard;
    NVNeventBuilder builder;
    NVNevent event;
    API(nvnEventBuilderSetDefaults)(&builder);
    API(nvnEventBuilderSetStorage)(&builder, pool, results + 4);
    assert(API(nvnEventInitialize)(&event, &builder));
    NVNcommandBuffer cb;
    assert(API(nvnCommandBufferInitialize)(&cb, device));
    API(nvnCommandBufferAddCommandMemory)(&cb, pool, 0x1000, 256);
    API(nvnCommandBufferBeginRecording)(&cb);
    API(nvnCommandBufferSignalEvent)(&cb, &event, NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 7);
    NVNcommandHandle first = API(nvnCommandBufferEndRecording)(&cb);
    API(nvnCommandBufferBeginRecording)(&cb);
    API(nvnCommandBufferSignalEvent)(&cb, &event, NVN_EVENT_SIGNAL_MODE_ADD, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 4);
    NVNcommandHandle inner = API(nvnCommandBufferEndRecording)(&cb);
    NVNcommandHandle after_end = record_value(&cb, addr + 20, 51);
    assert(after_end == inner + 24 + sizeof(op_hdr));

    // Seed an executable write at the stale fall-through address. The first
    // segment fits REPORT (24), CALL (16) and the refill JUMP (16).
    API(nvnCommandBufferAddCommandMemory)(&cb, pool, segment + 56, 128);
    NVNcommandHandle after_jump = record_value(&cb, addr + 24, 61);
    refill_context ctx = {pool, 0};
    API(nvnCommandBufferSetMemoryCallbackData)(&cb, &ctx);
    API(nvnCommandBufferSetMemoryCallback)(&cb, refill);
    API(nvnCommandBufferAddCommandMemory)(&cb, pool, segment, 72);
    API(nvnCommandBufferBeginRecording)(&cb);
    API(nvnCommandBufferReportValue)(&cb, 17, addr + 12);
    API(nvnCommandBufferCallCommands)(&cb, 1, &inner);
    API(nvnCommandBufferReportValue)(&cb, 23, addr + 16);
    NVNsync fence;
    assert(API(nvnSyncInitialize)(&fence, device));
    API(nvnCommandBufferFenceSync)(&cb, &fence, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle outer = API(nvnCommandBufferEndRecording)(&cb);
    assert(ctx.calls == 1 && after_jump == outer + 56);
    assert(API(nvnEventGetValue)(&event) == guard);
    API(nvnQueueSubmitCommands)(queue, 1, &first);
    API(nvnQueueSubmitCommands)(queue, 1, &outer);
    NVNsyncWaitResult waited = API(nvnSyncWait)(&fence, 5000000000ull);
    assert(waited == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED || waited == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    const uint32_t expected[] = {guard, 11, guard, 17, 23, guard, guard, guard};
    assert(API(nvnEventGetValue)(&event) == 11 && !memcmp(words, expected, sizeof expected));

    // Positive controls: both writes skipped above really do write when submitted.
    NVNcommandHandle controls[] = {after_end, after_jump};
    API(nvnQueueSubmitCommands)(queue, 2, controls);
    API(nvnQueueFinish)(queue);
    assert(words[5] == 51 && words[6] == 61);
    API(nvnSyncFinalize)(&fence);
    API(nvnCommandBufferFinalize)(&cb);
    API(nvnEventFinalize)(&event);
    puts("Public NVN event lists: 7 + 4 at the fence, refill and nested return preserved both guards");
}

#ifdef DM_VK_PUBLIC_BINDINGS
// Depth/stencil planes are copied separately by Vulkan. Maxwell Z24S8 places
// stencil in the low byte; ZF32_X24S8 stores the float before that byte.
static void check_packed_texels(NVNdevice *device, NVNqueue *queue, NVNmemoryPool *pool, uint8_t *storage) {
    const NVNformat formats[] = {NVN_FORMAT_STENCIL8, NVN_FORMAT_DEPTH24, NVN_FORMAT_DEPTH24_STENCIL8, NVN_FORMAT_DEPTH32F_STENCIL8};
    for (size_t i = 0; i < sizeof formats / sizeof *formats; ++i) {
        NVNtexture tex;
        NVNtextureBuilder tb;
        API(nvnTextureBuilderSetDefaults)(&tb);
        API(nvnTextureBuilderSetDevice)(&tb, device);
        API(nvnTextureBuilderSetFormat)(&tb, formats[i]);
        API(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
        API(nvnTextureBuilderSetSize2D)(&tb, 4, 2);
        API(nvnTextureBuilderSetStorage)(&tb, pool, 0x4000);
        assert(API(nvnTextureInitialize)(&tex, &tb));
        NVNcommandBuffer cb;
        assert(API(nvnCommandBufferInitialize)(&cb, device));
        API(nvnCommandBufferAddCommandMemory)(&cb, pool, 0, 0x4000);
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSetRenderTargets)(&cb, 0, NULL, NULL, &tex, NULL);
        API(nvnCommandBufferSetScissor)(&cb, 0, 0, 4, 2);
        API(nvnCommandBufferClearDepthStencil)(&cb, 1, 1, 0x5c, 0xff);
        NVNcommandHandle commands = API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue, 1, &commands);
        unsigned bpp = nvn_fmt_bpb(formats[i], NULL, NULL);
        uint8_t cleared[8] = {0}, edit[8] = {0};
        if (formats[i] == NVN_FORMAT_STENCIL8) {
            cleared[0] = 0x5c; edit[0] = 0x93;
        } else if (formats[i] == NVN_FORMAT_DEPTH32F_STENCIL8) {
            float before = 1, after = 0.375f;
            memcpy(cleared, &before, 4); memcpy(edit, &after, 4);
            cleared[4] = 0x5c; edit[4] = 0x93;
        } else {
            int stencil = formats[i] == NVN_FORMAT_DEPTH24_STENCIL8;
            uint32_t before = 0xffffff00u | (stencil ? 0x5c : 0), after = 0x23456700u | (stencil ? 0x93 : 0);
            memcpy(cleared, &before, 4); memcpy(edit, &after, 4);
        }
        NVNcopyRegion all = {0, 0, 0, 4, 2, 1}, one = {2, 1, 0, 1, 1, 1};
        uint8_t got[100], expected[100];
        memset(expected, 0xbb, sizeof expected);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 4; ++x) memcpy(expected + 3 + 41 * y + bpp * x, cleared, bpp);
        for (int pass = 0; pass < 2; ++pass) {
            if (pass) {
                API(nvnTextureWriteTexels)(&tex, NULL, &one, edit);
                memcpy(expected + 3 + 41 + bpp * 2, edit, bpp);
            }
            memset(got, 0xbb, sizeof got);
            API(nvnTextureReadTexelsStrided)(&tex, NULL, &all, got + 3, 41, 90);
            for (size_t k = 0; k < sizeof got; ++k) if (got[k] != expected[k])
                die("packed format %#x pass %d byte %zu returned %#x expected %#x", formats[i], pass, k, got[k], expected[k]);
        }
        // Copy a subregion through the public command stream. The second GPU
        // copy consumes the packed result before the queue publishes CPU bytes.
        enum { input = 0x9000, output = 0x9200, copied = 0x9400, row = 37, size = 96 };
        NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(pool);
        memset(storage+input,0xa5,size); memset(storage+output,0xc3,size); memset(storage+copied,0x77,size);
        uint8_t want[size]; memset(want,0xc3,sizeof want);
        NVNcopyRegion upload = {1,0,0,3,2,1};
        for (int y=0; y<2; ++y) for (int x=0; x<4; ++x) {
            uint8_t value[8]; memcpy(value,cleared,bpp);
            if (x) {
                unsigned k=4*y+x;
                if (formats[i]==NVN_FORMAT_STENCIL8) value[0]=17*k;
                else if (formats[i]==NVN_FORMAT_DEPTH32F_STENCIL8) {
                    float depth=(float)k/8; memcpy(value,&depth,4); value[4]=17*k;
                } else {
                    uint32_t word=(((0x173aa1u*k)&0xffffffu)<<8) | (formats[i]==NVN_FORMAT_DEPTH24_STENCIL8 ? 17*k : 0);
                    memcpy(value,&word,4);
                }
                memcpy(storage+input+y*row+(x-1)*bpp,value,bpp);
            }
            memcpy(want+3+y*row+x*bpp,value,bpp);
        }
        // This texel will come from an earlier GPU command, even when the
        // rest of the same source row comes directly from guest memory.
        memcpy(storage+input+128,storage+input,bpp);
        memset(storage+input,0,bpp);
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferCopyBufferToBuffer)(&cb,addr+input+128,addr+input,bpp,0);
        API(nvnCommandBufferSetCopyRowStride)(&cb,row);
        API(nvnCommandBufferCopyBufferToTexture)(&cb,addr+input,&tex,NULL,&upload,0);
        API(nvnCommandBufferCopyTextureToBuffer)(&cb,&tex,NULL,&all,addr+output+3,0);
        API(nvnCommandBufferCopyBufferToBuffer)(&cb,addr+output,addr+copied,size,0);
        commands=API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue,1,&commands);
        API(nvnQueueFinish)(queue);
        for (size_t k=0; k<size; ++k)
            if (want[k]!=storage[output+k] || want[k]!=storage[copied+k])
                die("packed transfer format %#x byte %zu: texture %02x, same-list buffer %02x, expected %02x",
                    formats[i],k,storage[output+k],storage[copied+k],want[k]);
        memset(got,0xc3,size);
        API(nvnTextureReadTexelsStrided)(&tex,NULL,&all,got+3,row,size);
        assert(!memcmp(want,got,size));
        // A fresh texture can get its first contents from the guest pool,
        // before any GPU command has written its Vulkan backing.
        API(nvnTextureFinalize)(&tex);
        assert(API(nvnTextureInitialize)(&tex,&tb));
        API(nvnTextureWriteTexelsStrided)(&tex,NULL,&all,want+3,row,size);
        memset(storage+output,0xc3,size);
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSetCopyRowStride)(&cb,row);
        API(nvnCommandBufferCopyTextureToBuffer)(&cb,&tex,NULL,&all,addr+output+3,0);
        commands=API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue,1,&commands);
        API(nvnQueueFinish)(queue);
        for (size_t k=0; k<size; ++k) if (want[k]!=storage[output+k])
            die("initial packed upload format %#x byte %zu: got %02x expected %02x",formats[i],k,storage[output+k],want[k]);
        API(nvnCommandBufferFinalize)(&cb);
        API(nvnTextureFinalize)(&tex);
    }
    puts("Public depth/stencil texels: packed upload, same-list GPU readback, CPU reads and padding verified");
}

// Hold publication of the first row after the GPU fence. A CPU writer can
// otherwise pass consistently just because the completion thread ran first.
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    const void *row;
    int entered, release;
} publish_gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0, 0};

static void delay_first_row(void *addr, size_t size) {
    if (size != 28) return;
    pthread_mutex_lock(&publish_gate.mutex);
    if (addr == publish_gate.row) {
        publish_gate.entered = 1;
        pthread_cond_broadcast(&publish_gate.cond);
        while (!publish_gate.release) pthread_cond_wait(&publish_gate.cond, &publish_gate.mutex);
    }
    pthread_mutex_unlock(&publish_gate.mutex);
}

static void *release_first_row(void *unused) {
    (void)unused;
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_nsec += 200000000;
    if (end.tv_nsec >= 1000000000) { end.tv_nsec -= 1000000000; ++end.tv_sec; }
    pthread_mutex_lock(&publish_gate.mutex);
    while (!publish_gate.release) {
        if (pthread_cond_timedwait(&publish_gate.cond, &publish_gate.mutex, &end) == ETIMEDOUT) break;
    }
    publish_gate.release = 1;
    pthread_cond_broadcast(&publish_gate.cond);
    pthread_mutex_unlock(&publish_gate.mutex);
    return NULL;
}

static void check_linear_transfer(NVNdevice *device, NVNqueue *queue, NVNmemoryPool *pool, uint8_t *storage) {
    enum { texture_offset = 0x4000, read_offset = 0xa000, w = 7, h = 3 };
    NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(pool);
    dm_guest_write_prep_register(delay_first_row);
    NVNsync fence;
    assert(API(nvnSyncInitialize)(&fence, device));
    NVNcommandBuffer inner;
    assert(API(nvnCommandBufferInitialize)(&inner, device));
    API(nvnCommandBufferAddCommandMemory)(&inner, pool, 0x3000, 0x1000);
    for (int rt = 0; rt < 2; ++rt) {
        NVNtextureBuilder tb;
        API(nvnTextureBuilderSetDefaults)(&tb);
        API(nvnTextureBuilderSetDevice)(&tb, device);
        API(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
        API(nvnTextureBuilderSetSize2D)(&tb, w, h);
        API(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
        API(nvnTextureBuilderSetFlags)(&tb, rt ? NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET : NVN_TEXTURE_FLAGS_LINEAR);
        if (!rt) API(nvnTextureBuilderSetStride)(&tb, 64);
        API(nvnTextureBuilderSetStorage)(&tb, pool, texture_offset);
        size_t stride = API(nvnTextureBuilderGetStorageSize)(&tb) / h;
        assert(stride > 4 * w && stride <= 128);
        NVNtexture tex;
        dm_guest_write_prepare(storage + texture_offset, stride * h);
        memset(storage + texture_offset, 0x5e, stride * h);
        assert(API(nvnTextureInitialize)(&tex, &tb));
        uint8_t pattern[h][w * 4];
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w * 4; ++x)
            pattern[y][x] = (uint8_t)(1 + y * 40 + x);
        NVNcopyRegion region = {0, 0, 0, w, h, 1};
        API(nvnTextureWriteTexels)(&tex, NULL, &region, pattern);
        NVNcommandBuffer cb;
        assert(API(nvnCommandBufferInitialize)(&cb, device));
        API(nvnCommandBufferAddCommandMemory)(&cb, pool, 0, 0x3000);
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSetCopyRowStride)(&cb, 0);
        API(nvnCommandBufferSetCopyImageStride)(&cb, 0);
        API(nvnCommandBufferCopyTextureToBuffer)(&cb, &tex, NULL, &region, addr + read_offset, 0);
        NVNcommandHandle commands = API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue, 1, &commands);
        API(nvnQueueFinish)(queue);
        for (size_t i = 0; i < sizeof pattern; ++i) if (storage[read_offset + i] != ((uint8_t *)pattern)[i])
            die("linear GPU upload, render-target flag %d, byte %zu: %u expected %u", rt, i, storage[read_offset + i], ((uint8_t *)pattern)[i]);
        const NVNtexture *target = &tex;
        API(nvnCommandBufferBeginRecording)(&inner);
        API(nvnCommandBufferReportValue)(&inner, 37 + rt, addr + read_offset + 128);
        API(nvnCommandBufferFenceSync)(&inner, &fence, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
        NVNcommandHandle nested = API(nvnCommandBufferEndRecording)(&inner);
        // Recording leaves the first generation unsignaled and the previous
        // generation signaled until submission actually reaches this call.
        assert(API(nvnSyncWait)(&fence, 0) == (rt ? NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED));
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferSetRenderTargets)(&cb, 1, &target, NULL, NULL, NULL);
        API(nvnCommandBufferSetScissor)(&cb, 0, 0, w, h);
        float green[] = {0, 1, 0, 1};
        API(nvnCommandBufferClearColor)(&cb, 0, green, 15);
        API(nvnCommandBufferCallCommands)(&cb, 1, &nested);
        commands = API(nvnCommandBufferEndRecording)(&cb);
        pthread_mutex_lock(&publish_gate.mutex);
        publish_gate.entered = publish_gate.release = 0;
        publish_gate.row = storage + texture_offset;
        pthread_mutex_unlock(&publish_gate.mutex);
        API(nvnQueueSubmitCommands)(queue, 1, &commands);
        pthread_mutex_lock(&publish_gate.mutex);
        while (!publish_gate.entered) pthread_cond_wait(&publish_gate.cond, &publish_gate.mutex);
        pthread_mutex_unlock(&publish_gate.mutex);
        assert(API(nvnSyncWait)(&fence, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        assert(API(nvnSyncWait)(&fence, 1000000) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        if (!rt && getenv("DM_VK_TEST_LONG_FENCE")) {
            uint64_t start = now_ns(), timeout = 60001000000ull;
            assert(API(nvnSyncWait)(&fence, timeout) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
            assert(now_ns() - start >= timeout);
            puts("Public finite guest fence wait above one minute expired");
        }
        pthread_t release;
        assert(!pthread_create(&release, NULL, release_first_row, NULL));
        NVNcopyRegion one = {2, 1, 0, 1, 1, 1};
        uint8_t purple[] = {128, 0, 192, 255};
        API(nvnTextureWriteTexels)(&tex, NULL, &one, purple);
        pthread_mutex_lock(&publish_gate.mutex);
        publish_gate.release = 1;
        pthread_cond_broadcast(&publish_gate.cond);
        pthread_mutex_unlock(&publish_gate.mutex);
        assert(!pthread_join(release, NULL));
        NVNsyncWaitResult waited = API(nvnSyncWait)(&fence, UINT64_MAX);
        assert(waited == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED || waited == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
        assert(*(uint32_t *)(storage + read_offset + 128) == 37u + rt);
        assert(API(nvnSyncWait)(&fence, 0) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
        pthread_mutex_lock(&publish_gate.mutex);
        publish_gate.row = NULL;
        pthread_mutex_unlock(&publish_gate.mutex);
        uint8_t pixel[] = {0, 255, 0, 255};
        for (int y = 0; y < h; ++y) for (size_t x = 0; x < stride; ++x) {
            uint8_t want = x < 4 * w ? pixel[x % 4] : 0x5e;
            if (y == 1 && x / 4 == 2) want = purple[x % 4];
            if (storage[texture_offset + stride * y + x] != want) die("linear GPU writeback flag %d row %d byte %zu", rt, y, x);
        }
        API(nvnCommandBufferFinalize)(&cb);
        API(nvnTextureFinalize)(&tex);
    }
    API(nvnCommandBufferFinalize)(&inner);
    API(nvnSyncFinalize)(&fence);
    puts("Public nested fence rearmed on submission, timed out during guest writeback and published both generations");
}
#endif

// The same command list and expected bytes run on Metal and Vulkan.
static void check_subregion_transfers(NVNdevice *device, NVNqueue *queue, NVNmemoryPool *pool, uint8_t *storage) {
    enum { input=0xa000, output=0xa200, consumed=0xa400, donor=0xa600, row=32, image=96, size=192 };
    NVNbufferAddress addr=API(nvnMemoryPoolGetBufferAddress)(pool);
    for (int mode=0; mode<3; ++mode) {
        int volume=mode==1, bc=mode==2;
        unsigned w=bc ? 16 : 8, h=bc ? 16 : 8;
        NVNtextureBuilder tb;
        API(nvnTextureBuilderSetDefaults)(&tb); API(nvnTextureBuilderSetDevice)(&tb,device);
        API(nvnTextureBuilderSetFormat)(&tb,bc ? NVN_FORMAT_RGBA_DXT1 : NVN_FORMAT_RGBA8);
        API(nvnTextureBuilderSetTarget)(&tb,volume ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D_ARRAY);
        API(nvnTextureBuilderSetSize3D)(&tb,w,h,8); API(nvnTextureBuilderSetLevels)(&tb,2);
        API(nvnTextureBuilderSetStorage)(&tb,pool,0x4000);
        assert(API(nvnTextureBuilderGetStorageSize)(&tb)<=input-0x4000);
        NVNtexture tex; assert(API(nvnTextureInitialize)(&tex,&tb));
        NVNtextureView view; API(nvnTextureViewSetDefaults)(&view); API(nvnTextureViewSetLevels)(&view,1,1);
        if (!volume) API(nvnTextureViewSetLayers)(&view,1,3);
        NVNcopyRegion region={bc?4:1,0,1,bc?4:2,bc?8:2,2};
        uint8_t wanted[size]; memset(wanted,0xee,size);
        memset(storage+input,0x55,size); memset(storage+output,0xee,size); memset(storage+consumed,0x13,size);
        API(nvnTextureWriteTexelsStrided)(&tex,&view,&region,storage+input,row,image);
        for (int z=0; z<2; ++z) for (int y=0; y<2; ++y) for (int x=0; x<8; ++x) {
            size_t k=z*image+y*row+x;
            wanted[k]=storage[input+k]=17+61*z+13*y+3*x;
        }
        memcpy(storage+donor,wanted+image,8); memset(storage+input+image,0,8);
        NVNcommandBuffer cb; assert(API(nvnCommandBufferInitialize)(&cb,device));
        API(nvnCommandBufferAddCommandMemory)(&cb,pool,0,0x4000);
        API(nvnCommandBufferBeginRecording)(&cb);
        API(nvnCommandBufferCopyBufferToBuffer)(&cb,addr+donor,addr+input+image,8,0);
        API(nvnCommandBufferSetCopyRowStride)(&cb,row); API(nvnCommandBufferSetCopyImageStride)(&cb,image);
        API(nvnCommandBufferCopyBufferToTexture)(&cb,addr+input,&tex,&view,&region,0);
        API(nvnCommandBufferCopyTextureToBuffer)(&cb,&tex,&view,&region,addr+output,0);
        API(nvnCommandBufferCopyBufferToBuffer)(&cb,addr+output,addr+consumed,size,0);
        NVNcommandHandle commands=API(nvnCommandBufferEndRecording)(&cb);
        API(nvnQueueSubmitCommands)(queue,1,&commands); API(nvnQueueFinish)(queue);
        for (size_t k=0; k<size; ++k) if (wanted[k]!=storage[output+k] || wanted[k]!=storage[consumed+k])
            die("pitched transfer mode %d byte %zu: copied %02x consumed %02x expected %02x",mode,k,storage[output+k],storage[consumed+k],wanted[k]);
        if (!volume) {
            uint8_t bytes[size]; memset(bytes,0xee,sizeof bytes);
            API(nvnTextureReadTexelsStrided)(&tex,&view,&region,bytes,row,image);
            for (size_t k=0; k<size; ++k) if (wanted[k]!=bytes[k])
                die("pitched transfer storage mode %d byte %zu: got %02x expected %02x",mode,k,bytes[k],wanted[k]);
        }
        API(nvnCommandBufferFinalize)(&cb); API(nvnTextureFinalize)(&tex);
    }
    puts("Public NVN pitched GPU transfers: array mip, 3D slices, original BC blocks and same-list buffer consumer passed");
}

typedef struct {
    const NVNtexture *texture;
    const NVNtextureView *view;
    NVNcopyRegion region;
    uint8_t *dst;
    size_t row, image;
} texture_reader;

static void *read_texture_on_worker(void *arg) {
    texture_reader *read = arg;
    API(nvnTextureReadTexelsStrided)(read->texture, read->view, &read->region, read->dst, read->row, read->image);
    return NULL;
}

// A CPU-only texture can be read on a guest worker before any submission or
// registration. Use different source and destination pitches, including a mip
// and layer offset, and check each byte the caller expects to keep.
static void check_cpu_texture_storage(NVNdevice *device) {
    static _Alignas(16384) uint8_t memory[0x10000];
    NVNmemoryPool pool;
    NVNmemoryPoolBuilder pb;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);
    API(nvnMemoryPoolBuilderSetDevice)(&pb, device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, memory, sizeof memory);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    assert(API(nvnMemoryPoolInitialize)(&pool, &pb));
    for (int mode = 0; mode < 4; ++mode) {
        int volume = mode == 1, compressed = mode == 2, linear = mode == 3;
        NVNtextureBuilder tb;
        API(nvnTextureBuilderSetDefaults)(&tb);
        API(nvnTextureBuilderSetDevice)(&tb, device);
        API(nvnTextureBuilderSetFormat)(&tb, compressed ? NVN_FORMAT_RGTC1_UNORM : NVN_FORMAT_RGBA8);
        API(nvnTextureBuilderSetTarget)(&tb, linear ? NVN_TEXTURE_TARGET_2D : volume ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D_ARRAY);
        API(nvnTextureBuilderSetSize3D)(&tb, 32, 24, linear ? 1 : 8);
        API(nvnTextureBuilderSetLevels)(&tb, linear ? 1 : 2);
        if (linear) {
            API(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_LINEAR);
            API(nvnTextureBuilderSetStride)(&tb, 192);
        }
        API(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
        assert(API(nvnTextureBuilderGetStorageSize)(&tb) <= sizeof memory);
        NVNtexture texture;
        assert(API(nvnTextureInitialize)(&texture, &tb));
        NVNtextureView view;
        API(nvnTextureViewSetDefaults)(&view);
        API(nvnTextureViewSetLevels)(&view, linear ? 0 : 1, 1);
        if (!linear && !volume) API(nvnTextureViewSetLayers)(&view, 2, 4);
        NVNcopyRegion region = {compressed ? 4 : 3, compressed ? 4 : 2, linear ? 0 : 1,
                                compressed ? 8 : 5, compressed ? 8 : 3, linear ? 1 : 2};
        size_t width = compressed ? 16 : 20, rows = compressed ? 2 : 3;
        uint8_t src[256], dst[256], expected[256];
        memset(src, 0xaf, sizeof src); memset(expected, 0xcb, sizeof expected); memset(dst, 0xcb, sizeof dst);
        for (int z = 0; z < region.depth; ++z) for (size_t y = 0; y < rows; ++y)
            for (size_t x = 0; x < width; ++x)
                src[7 + 107 * z + 31 * y + x] = expected[9 + 113 * z + 34 * y + x] = (uint8_t)(3 + x + 41 * y + 101 * z);
        API(nvnTextureWriteTexelsStrided)(&texture, &view, &region, src + 7, 31, 107);
        API(nvnTextureReadTexelsStrided)(&texture, &view, &region, dst + 9, 34, 113);
        for (size_t i = 0; i < sizeof dst; ++i) if (dst[i] != expected[i])
            die("CPU texture storage %d byte %zu: %u expected %u", mode, i, dst[i], expected[i]);
        API(nvnTextureFinalize)(&texture);
    }
    API(nvnMemoryPoolFinalize)(&pool);
    puts("Public CPU texture storage: pitched arrays, volumes, compressed blocks and linear rows, including all guards");
}

#ifdef DM_VK_PUBLIC_BINDINGS
#include "../nvn/vulkan/tests/public_bindings.inc"
#include "../nvn/vulkan/tests/public_buffers.inc"
#include "../nvn/vulkan/tests/public_feedback.inc"
#endif

#include "recorded_draw_texture.h"
#include "recorded_sampler_reduction.h"
#include "recorded_sparse_texture.h"
#include "recorded_sparse_write.h"
#include "recorded_texture_alias.h"
#include "recorded_texture_srgb.h"
#include "recorded_query_lifetime.h"
#include "recorded_feedback_control.h"

// Reinitializing a network at the same guest address starts a new temporal
// history. Commands that were already recorded keep their original identity.
static void check_dlss_network_identity(NVNdevice *device, NVNmemoryPool *pool) {
    dlss_net_t network;
    dlss_builder_t builder={.w=256,.h=128,.flags=DLSS_F_DEPTH_INVERTED};
    dlss_params_t params={.iw=128,.ih=64};
    __typeof__(&h_nvnDlssNetworkInitialize) init=bootstrap("nvnDlssNetworkInitialize");
    __typeof__(&h_nvnDlssNetworkFinalize) fini=bootstrap("nvnDlssNetworkFinalize");
    __typeof__(&h_nvnCommandBufferEvaluateDlss) eval=bootstrap("nvnCommandBufferEvaluateDlss");
    NVNcommandBuffer cb;
    assert(API(nvnCommandBufferInitialize)(&cb,device));
    API(nvnCommandBufferAddCommandMemory)(&cb,pool,0,0x4000);
    const p_dlss *records[3];
    for (int i=0;i<3;i++) {
        if (i==2) { fini(&network); builder.w=384; }
        if (i!=1) assert(init(&network,&builder));
        API(nvnCommandBufferBeginRecording)(&cb);
        eval(&cb,&network,&params);
        const op_hdr *head=(const op_hdr *)API(nvnCommandBufferEndRecording)(&cb);
        assert(head->op==OP_DLSS);
        records[i]=(const p_dlss *)(head+1);
    }
    assert(records[0]->net && records[0]->net==records[1]->net && records[1]->net!=records[2]->net);
    assert(records[0]->ow==256 && records[2]->ow==384 && records[2]->iw==128);
    fini(&network);
    API(nvnCommandBufferFinalize)(&cb);
    puts("Reusing an NVN2 network address preserves the identities and sizes of earlier recordings");
}

int main(void) {
    load_bootstrap();
    enum { pool_size = 0x10000, cmd_size = 0x4000, tex_offset = 0x4000, dst_offset = 0x8000,
           row_pitch = 24, image_pitch = 80, dst_bytes = 176, guard = 8 };
    static _Alignas(16384) uint8_t storage[pool_size];
    NVNdevice device;
    NVNdeviceBuilder db;
    API(nvnDeviceBuilderSetDefaults)(&db);
    assert(API(nvnDeviceInitialize)(&device, &db));
    check_cpu_texture_storage(&device);
    NVNqueue queue;
    NVNqueueBuilder qb;
    API(nvnQueueBuilderSetDevice)(&qb, &device);
    API(nvnQueueBuilderSetDefaults)(&qb);
    assert(API(nvnQueueInitialize)(&queue, &qb));
    if (getenv("DM_TEST_TEXTURE_SRGB")) {
        check_texture_srgb_aliases(&device,&queue);
        API(nvnQueueFinalize)(&queue);API(nvnDeviceFinalize)(&device);return 0;
    }
    if (getenv("DM_TEST_TEXTURE_ALIASES")) {
        check_texture_object_aliases(&device,&queue);
        API(nvnQueueFinalize)(&queue);API(nvnDeviceFinalize)(&device);return 0;
    }
#ifdef __ANDROID__
    if (getenv("DM_VK_TEST_PUBLIC_REDUCTION")) {
        check_sampler_reduction(&device,&queue,NVN_FORMAT_R32F);
        check_sampler_reduction(&device,&queue,NVN_FORMAT_RGBA8);
        API(nvnQueueFinalize)(&queue);API(nvnDeviceFinalize)(&device);return 0;
    }
    if (getenv("DM_VK_TEST_SPARSE_WRITES")) {
        check_sparse_textures(&device,&queue);
        check_sparse_encoded_writes(&device,&queue);
        API(nvnQueueFinalize)(&queue);API(nvnDeviceFinalize)(&device);return 0;
    }
#endif

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
    API(nvnQueueSubmitCommands)(&queue, 1, &handle);
    uint8_t direct[sizeof expected];
    memset(direct, 0xcc, sizeof direct);
    texture_reader read = {&tex, &view, sub, direct + guard, row_pitch, image_pitch};
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, read_texture_on_worker, &read));
    assert(!pthread_join(worker, NULL));
    for (size_t i = 0; i < sizeof direct; ++i) if (direct[i] != expected[i])
        die("CPU read of GPU texture byte %zu: %u expected %u", i, direct[i], expected[i]);
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
#ifdef DM_VK_PUBLIC_BINDINGS
    if (getenv("DM_VK_DIR")) {
        check_texture_bindings(&device, &queue);
        check_buffer_bindings(&device, &queue);
    }
#endif
    // Patch only one texel per layer of a live GPU-written target. Read
    // through the public CPU and GPU paths; the neighboring red/blue texels
    // and all destination padding must still contain their earlier bytes.
#ifdef DM_VK_PUBLIC_BINDINGS
    NVNcopyRegion edit = {3, 2, 0, 1, 1, 2};
    uint8_t brush[20] = {0, 255, 0, 255, [16] = 255, 255, 0, 255};
    API(nvnTextureWriteTexelsStrided)(&tex, &view, &edit, brush, 8, 16);
    for (int z = 0; z < 2; ++z) memcpy(expected + guard + z * image_pitch + row_pitch + 4, brush + z * 16, 4);
    memset(direct, 0xcc, sizeof direct);
    API(nvnTextureReadTexelsStrided)(&tex, &view, &sub, direct + guard, row_pitch, image_pitch);
    assert(!memcmp(direct, expected, sizeof direct));
    API(nvnCommandBufferBeginRecording)(&cb);
    API(nvnCommandBufferSetCopyRowStride)(&cb, row_pitch);
    API(nvnCommandBufferSetCopyImageStride)(&cb, image_pitch);
    API(nvnCommandBufferCopyTextureToBuffer)(&cb, &tex, &view, &sub, addr + dst_offset + guard, 0);
    handle = API(nvnCommandBufferEndRecording)(&cb);
    memset(storage + dst_offset, 0xcc, sizeof expected);
    API(nvnQueueSubmitCommands)(&queue, 1, &handle);
    API(nvnQueueFinish)(&queue);
    assert(!memcmp(storage + dst_offset, expected, sizeof expected));
    puts("Two partial CPU writes updated the live GPU texture and preserved neighboring texels and padding");
#endif
    NVNtexturePool descriptors;
    assert(API(nvnTexturePoolInitialize)(&descriptors, &pool, 0xc000, 257));
    API(nvnTexturePoolRegisterImage)(&descriptors, 256, &tex, &view);
    API(nvnTexturePoolRegisterImage)(&descriptors, 256, NULL, NULL);
    API(nvnTexturePoolRegisterTexture)(&descriptors, 256, &tex, NULL);
    API(nvnTexturePoolFinalize)(&descriptors);
    API(nvnQueueFinish)(&queue);
    check_event_lists(&device, &queue, &pool, storage);
    API(nvnSyncFinalize)(&sync);
    API(nvnCommandBufferFinalize)(&cb);
    API(nvnTextureFinalize)(&tex);
    check_subregion_transfers(&device,&queue,&pool,storage);
    check_draw_texture(&device, &queue);
    check_sampler_reduction(&device, &queue, NVN_FORMAT_R32F);
    check_sampler_reduction(&device, &queue, NVN_FORMAT_RGBA8);
    check_sparse_textures(&device, &queue);
    check_texture_object_aliases(&device,&queue);
    check_texture_srgb_aliases(&device,&queue);
    check_query_lifetimes(&device,&queue,&qb);
    check_feedback_controls(&device, &queue);
    check_dlss_network_identity(&device,&pool);
#ifdef DM_VK_PUBLIC_BINDINGS
    check_linear_transfer(&device, &queue, &pool, storage);
    check_packed_texels(&device, &queue, &pool, storage);
#endif
    API(nvnQueueFinalize)(&queue);
    API(nvnMemoryPoolFinalize)(&pool);
    API(nvnDeviceFinalize)(&device);
    puts("Public NVN recorder returned both layers, preserved guards and padding, and reported timestamps on the guest clock");
}
