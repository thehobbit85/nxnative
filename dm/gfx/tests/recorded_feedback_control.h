// A feedback control block contains four byte cursors followed by indirect
// draw arguments. A saved block may immediately feed another GPU command.
static void check_feedback_controls(NVNdevice *device, NVNqueue *queue) {
    enum { words = 8, cmd_bytes = 0x4000, stride = 64 };
    static _Alignas(16384) uint8_t bytes[0x8000];
    const uint32_t initial[words] = {0,0,0,0,0,1,0,0};
    const uint32_t saved[words] = {32,64,96,128,8,1,0,0};
    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);
    API(nvnMemoryPoolBuilderSetDevice)(&pb, device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, bytes, sizeof bytes);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    assert(API(nvnMemoryPoolInitialize)(&pool, &pb));
    uint8_t *data = bytes + cmd_bytes;
    NVNbufferAddress addr = API(nvnMemoryPoolGetBufferAddress)(&pool) + cmd_bytes;
    memset(data, 0xa5, 6 * stride);
    memcpy(data, saved, sizeof saved);
    NVNcommandBuffer cb;
    assert(API(nvnCommandBufferInitialize)(&cb, device));
    API(nvnCommandBufferAddCommandMemory)(&cb, &pool, 0, cmd_bytes);
    API(nvnCommandBufferBeginRecording)(&cb);
    API(nvnCommandBufferPauseTransformFeedback)(&cb, addr + stride);
    API(nvnCommandBufferCopyBufferToBuffer)(&cb, addr, addr + 2 * stride, sizeof saved, 0);
    API(nvnCommandBufferResumeTransformFeedback)(&cb, addr + 2 * stride);
    API(nvnCommandBufferPauseTransformFeedback)(&cb, 0);
    NVNcommandHandle nested = API(nvnCommandBufferEndRecording)(&cb);
    API(nvnCommandBufferBeginRecording)(&cb);
    API(nvnCommandBufferBeginTransformFeedback)(&cb, addr + 5 * stride);
    API(nvnCommandBufferCallCommands)(&cb, 1, &nested);
    API(nvnCommandBufferResumeTransformFeedback)(&cb, 0);
    API(nvnCommandBufferEndTransformFeedback)(&cb, addr + 3 * stride);
    NVNcommandHandle first = API(nvnCommandBufferEndRecording)(&cb);
    API(nvnQueueSubmitCommands)(queue, 1, &first);

    API(nvnCommandBufferBeginRecording)(&cb);
    API(nvnCommandBufferBeginTransformFeedback)(&cb, 0);
    API(nvnCommandBufferPauseTransformFeedback)(&cb, 0);
    API(nvnCommandBufferResumeTransformFeedback)(&cb, addr + 3 * stride);
    API(nvnCommandBufferEndTransformFeedback)(&cb, addr + 4 * stride);
    NVNcommandHandle second = API(nvnCommandBufferEndRecording)(&cb);
    API(nvnQueueSubmitCommands)(queue, 1, &second);
    API(nvnQueueFinish)(queue);
    for (unsigned i = 0; i < 6; i++) {
        const uint32_t *expected = i == 1 ? initial : saved;
        size_t written = i == 5 ? 0 : sizeof saved;
        if (written && memcmp(data + i * stride, expected, written)) {
            fprintf(stderr, "feedback block %u:", i);
            for (unsigned j = 0; j < words; j++)
                fprintf(stderr, " %08x", ((const uint32_t *)(data + i * stride))[j]);
            fputc('\n', stderr);
            abort();
        }
        for (size_t j = written; j < stride; j++) assert(data[i * stride + j] == 0xa5);
    }
#ifdef DM_VK_PUBLIC_BINDINGS
    check_feedback_streams(device, queue, &cb, addr, data);
#endif
    API(nvnCommandBufferFinalize)(&cb);
    API(nvnMemoryPoolFinalize)(&pool);
    puts("Public NVN transform feedback controls: GPU-ordered begin, pause, resume and end with guarded indirect arguments");
}
