// Execute the shared Apple capture entry for a bytewise Vulkan comparison.
// Compile test_android_vertex_capture.py --metal-fixture's output to a metallib.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { WORDS = 144, PREFIX = 16, RECORDS = 8, TOTAL = PREFIX + RECORDS * WORDS + 16 };
typedef struct __attribute__((aligned(16))) {
    uint64_t dst, padding;
    uint32_t info[4], stride;
    int16_t slot[WORDS];
} capture_control;
_Static_assert(offsetof(capture_control, info) == 16, "Metal uint4 alignment");
_Static_assert(offsetof(capture_control, slot) == 36, "shared capture slot offset");
_Static_assert(sizeof(capture_control) == 336, "Metal control block trailing alignment");

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 3) { fprintf(stderr, "usage: t_shader_capture_metal capture.metallib capture.bin\n"); return 2; }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr, "no Metal device\n"); return 77; }
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = [library newFunctionWithName:@"main0_gs"];
    pipeline.rasterizationEnabled = NO;
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
    if (!state) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    id<MTLBuffer> output = [device newBufferWithLength:TOTAL * 4 options:MTLResourceStorageModeShared];
    if (!output) return 1;
    memset(output.contents, 0xcc, TOTAL * 4);
    capture_control control = {.dst = output.gpuAddress + PREFIX * 4, .info = {3, 4, 2, 2}, .stride = WORDS};
    for (unsigned i = 0; i < WORDS; i++) control.slot[i] = (int16_t)i;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
    // This fixture uses a conventional Metal pass with an unused attachment;
    // only shader output storage is compared, not the host's Metal 4 encoder.
    MTLTextureDescriptor *texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:1 height:1 mipmapped:NO];
    texture.usage = MTLTextureUsageRenderTarget;
    pass.colorAttachments[0].texture = [device newTextureWithDescriptor:texture];
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionDontCare;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:state];
    [encoder useResource:output usage:MTLResourceUsageWrite stages:MTLRenderStageVertex];
    [encoder setVertexBytes:&control length:sizeof(control) atIndex:13];
    // Deliberately draw outside the capture ranges: only vertices 3..6 and
    // instances 2..3 may write, just as Vulkan's range-guard fixture checks.
    [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:10 instanceCount:4 baseInstance:1];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
    FILE *file = fopen(argv[2], "wb"); if (!file) return 2;
    int written = fwrite(output.contents, 4, TOTAL, file) == TOTAL;
    if (fclose(file) || !written) return 2;
    puts("Metal vertex capture: wrote 8 raw-word records and range canaries");
    return 0;
} }
