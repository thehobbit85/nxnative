// Execute shared dmsh_build.vs_tf; stdout is the Vulkan comparison corpus.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "feedback_corpus.h"
#include <stddef.h>
typedef struct __attribute__((aligned(16))) {
    uint32_t offset[4], args[4];
    uint64_t order;
    uint32_t first, count, base_instance, pad;
    uint64_t reserved;
} feedback_control;
_Static_assert(offsetof(feedback_control, order) == 32, "Metal feedback pointer offset");
_Static_assert(sizeof(feedback_control) == 64, "Metal feedback control size");

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: feedback_corpus_metal fixture-directory > corpus.bin\n"); return 2; }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); if (!device) return 77;
    NSError *error = nil;
    id<MTLRenderPipelineState> states[2][2];
    for (unsigned l = 0; l < 2; l++) for (unsigned ordered = 0; ordered < 2; ordered++) {
        NSString *path = [NSString stringWithFormat:@"%s/feedback%u_%u.metallib", argv[1], l, ordered];
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
        if (!library) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
        pipeline.vertexFunction = [library newFunctionWithName:@"main0_tf"];
        pipeline.rasterizationEnabled = NO; pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        states[l][ordered] = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
        if (!states[l][ordered]) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    }
    id<MTLBuffer> source = [device newBufferWithLength:FC_SOURCE_WORDS * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> order = [device newBufferWithLength:FC_ORDER_WORDS * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> outputs[4];
    if (!source || !order) return 1;
    for (unsigned b = 0; b < 4; b++) {
        outputs[b] = [device newBufferWithLength:FC_OUTPUT_WORDS * 4 options:MTLResourceStorageModeShared];
        if (!outputs[b]) return 1;
    }
    fc_inputs(source.contents, order.contents);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    unsigned cases = 0;
    for (unsigned l = 0; l < 2; l++) for (unsigned test = 0; test < FC_CASES; test++) {
        for (unsigned b = 0; b < 4; b++) memset(outputs[b].contents, 0xcc, FC_OUTPUT_WORDS * 4);
        feedback_control control = {.offset = {FC_PREFIX * 4, FC_PREFIX * 4, FC_PREFIX * 4, FC_PREFIX * 4},
            .args = {fc_count(test), 1, 0, 0}, .order = order.gpuAddress + FC_ORDER_PREFIX * 4, .count = FC_COUNT};
        id<MTLCommandBuffer> command = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
        pass.colorAttachments[0].texture = texture; pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionDontCare;
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:states[l][fc_ordered(test)]];
        [encoder setVertexBuffer:source offset:(FC_SOURCE_PREFIX + fc_first(test) * FC_WORDS) * 4 atIndex:0];
        [encoder useResource:order usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [encoder setVertexBytes:&control length:sizeof control atIndex:14];
        for (unsigned b = 0; b < 4; b++) if (fc_stride(l, b)) [encoder setVertexBuffer:outputs[b] offset:0 atIndex:8 + b];
        [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:FC_COUNT];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
        for (unsigned b = 0; b < 4; b++) {
            if (!fc_check(outputs[b].contents, l, b, test)) return 1;
            if (fwrite(outputs[b].contents, 4, FC_OUTPUT_WORDS, stdout) != FC_OUTPUT_WORDS) return 2;
            cases++;
        }
    }
    fprintf(stderr, "Metal feedback corpus: %u buffer cases passed\n", cases); return fflush(stdout) != 0;
} }
