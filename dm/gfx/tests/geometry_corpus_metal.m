// Metal reference for the identical exported-geometry raw-word corpus.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "geometry_corpus.h"
#include "geometry_bindings.h"
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: geometry_corpus_metal fixture-directory > corpus.bin\n"); return 2; }
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
    unsigned cases = 0; const unsigned counts[] = {0, 1, 33, 97};
    for (unsigned t = 0; t < 5; t++) for (unsigned inv = 0; inv < 2; inv++) for (unsigned inst = 1; inst <= 2; inst++)
    for (unsigned n = 0; n < 4; n++) for (unsigned base = 0; base < 2; base++) { @autoreleasepool {
        geometry_case c = {t, inv * 2 + 1, inst, counts[n], base * 4096}; gc_fill(c, mapped);
        id<MTLCommandBuffer> command = [queue commandBuffer]; id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipelines[inv][t]]; [encoder setBuffer:arguments[inv][t] offset:0 atIndex:0];
        for (unsigned i = 0; i < 5; i++) [encoder useResource:buffers[i] usage:i == 1 || i == 2 ? MTLResourceUsageWrite : MTLResourceUsageRead];
        [encoder dispatchThreadgroups:MTLSizeMake((c.primitives + 31) / 32 + 1, inst + 1, c.invocations) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
        if (gc_check(c, mapped[1], mapped[2], stdout)) return 1;
        cases++;
    } }
    fprintf(stderr, "Metal geometry corpus: %u cases passed\n", cases); return fflush(stdout) != 0;
} }
