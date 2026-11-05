// Shared Apple geometry -> ordered feedback reference for the Vulkan integration corpus.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "geometry_feedback_corpus.h"
#include <stddef.h>
typedef struct __attribute__((aligned(16))) {
    uint32_t offset[4], args[4]; uint64_t order;
    uint32_t first, count, base_instance, pad; uint64_t reserved;
} feedback_control;
_Static_assert(sizeof(feedback_control) == 64, "shared feedback control size");
#include "geometry_bindings.h"
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: geometry_feedback_metal fixture-directory > corpus.bin\n"); return 2; }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); if (!device) return 77;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const unsigned words[] = {GC_INPUT_WORDS, GC_VWORDS, GC_IWORDS, GC_INFO, GC_MAP};
    id<MTLBuffer> buffers[5]; uint32_t *mapped[5];
    for (unsigned i = 0; i < 5; i++) { buffers[i] = [device newBufferWithLength:words[i] * 4 options:MTLResourceStorageModeShared]; if (!buffers[i]) return 1; mapped[i] = buffers[i].contents; }
    id<MTLComputePipelineState> pipelines[2][5]; id<MTLBuffer> arguments[2][5];
    for (unsigned inv = 0; inv < 2; inv++) for (unsigned t = 0; t < 5; t++) {
        NSString *path = [@(argv[1]) stringByAppendingPathComponent:[NSString stringWithFormat:@"00000000feed000%u.gc%u.metallib", inv * 2 + 1, t]];
        NSError *error = nil; id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
        if (!library) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        id<MTLFunction> function = [library newFunctionWithName:@"main0"];
        pipelines[inv][t] = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipelines[inv][t]) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        id<MTLArgumentEncoder> encoder = [function newArgumentEncoderWithBufferIndex:0];
        arguments[inv][t] = [device newBufferWithLength:encoder.encodedLength options:MTLResourceStorageModeShared];
        [encoder setArgumentBuffer:arguments[inv][t] offset:0];
        for (unsigned i = 0; i < 5; i++) [encoder setBuffer:buffers[i] offset:0 atIndex:gc_metal_bindings[i]];
    }
    id<MTLBuffer> order = [device newBufferWithLength:GF_ORDER_WORDS * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:GF_OUTPUT_WORDS * 4 options:MTLResourceStorageModeShared];
    if (!order || !output) return 1;
    id<MTLRenderPipelineState> readers[2][5]; id<MTLBuffer> reader_arguments[2][5];
    for (unsigned inv = 0; inv < 2; inv++) for (unsigned t = 0; t < 5; t++) {
        NSString *path = [@(argv[1]) stringByAppendingPathComponent:[NSString stringWithFormat:@"00000000feed000%u.gr%u.feedback.metallib", inv * 2 + 1, t]];
        NSError *error = nil; id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
        if (!library) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        id<MTLFunction> function = [library newFunctionWithName:@"main0_tf"];
        MTLRenderPipelineDescriptor *description = [MTLRenderPipelineDescriptor new];
        description.vertexFunction = function; description.rasterizationEnabled = NO;
        description.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        readers[inv][t] = [device newRenderPipelineStateWithDescriptor:description error:&error];
        if (!readers[inv][t]) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
        reader_arguments[inv][t] = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
        [arguments setArgumentBuffer:reader_arguments[inv][t] offset:0];
        [arguments setBuffer:buffers[1] offset:0 atIndex:gc_reader_metal_binding];
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    unsigned cases = 0; const unsigned counts[] = {0, 1, 33, 97};
    for (unsigned t = 0; t < 5; t++) for (unsigned inv = 0; inv < 2; inv++) for (unsigned inst = 1; inst <= 2; inst++)
    for (unsigned n = 0; n < 4; n++) for (unsigned base = 0; base < 2; base++) { @autoreleasepool {
        geometry_case c = {t, inv * 2 + 1, inst, counts[n], base * 4096}; gc_fill(c, mapped); gf_fill(c, order.contents, output.contents);
        id<MTLCommandBuffer> command = [queue commandBuffer]; id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipelines[inv][t]]; [encoder setBuffer:arguments[inv][t] offset:0 atIndex:0];
        for (unsigned i = 0; i < 5; i++) [encoder useResource:buffers[i] usage:i == 1 || i == 2 ? MTLResourceUsageWrite : MTLResourceUsageRead];
        [encoder dispatchThreadgroups:MTLSizeMake((c.primitives + 31) / 32 + 1, inst + 1, c.invocations) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [encoder endEncoding];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new]; pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare; pass.colorAttachments[0].storeAction = MTLStoreActionDontCare;
        id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
        [render setRenderPipelineState:readers[inv][t]];
        [render setVertexBuffer:reader_arguments[inv][t] offset:0 atIndex:0];
        [render useResource:buffers[1] usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [render useResource:order usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [render setVertexBuffer:output offset:0 atIndex:8];
        feedback_control control = {.offset = {GF_PREFIX * 4, 0, 0, 0}, .args = {gf_records(c), 1, 0, 0},
            .order = order.gpuAddress + GF_ORDER_PREFIX * 4};
        [render setVertexBytes:&control length:sizeof control atIndex:14];
        [render drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:gf_records(c)];
        [render endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
        if (gc_check(c, mapped[1], mapped[2], NULL) || gf_check(c, output.contents, stdout)) return 1;
        cases++;
    } }
    fprintf(stderr, "Metal geometry feedback corpus: %u cases passed\n", cases); return fflush(stdout) != 0;
} }
