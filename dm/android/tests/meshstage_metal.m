// The unchanged Apple wrapper's memory prefix with a test-only raw output dump.
// Bounds determine host dispatch here; Android's shader-side bounds are tested
// independently by meshstage.c, which deliberately over-dispatches every case.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INPUT_WORDS 400u
#define OUTPUT_WORDS (13u + 35u * 424u + 17u)
static uint32_t bits(unsigned n) {
    const uint32_t values[] = {0x80000000, 0x7fc12345, 1, 0xff800000, 0x3f800000, 0xdeadbeef, 0x007fffff, 0};
    return values[n % 8];
}
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 3) return 2;
    unsigned variant = (unsigned)strtoul(argv[2], NULL, 10); if (variant > 4) return 2;
    const unsigned threads[] = {8, 129, 32, 8, 1};
    const unsigned words = variant == 2 || variant == 3 ? 424 : 20;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); if (!device) return 77;
    NSError *error = nil;
    NSString *path = [@(argv[1]) stringByAppendingPathComponent:[NSString stringWithFormat:@"00000000feed001%u.metallib", variant]];
    if (!path) return 2;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    id<MTLFunction> function = [library newFunctionWithName:@"main0"]; if (!function) return 1;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    if (pipeline.maxTotalThreadsPerThreadgroup < threads[variant]) return 77;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> input = [device newBufferWithLength:INPUT_WORDS * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:OUTPUT_WORDS * 4 options:MTLResourceStorageModeShared];
    if (!queue || !input || !output) return 1;
    uint32_t *source = input.contents;
    for (unsigned i = 0; i < INPUT_WORDS; i++) source[i] = bits(i);
    for (unsigned g = 0; g < 35; g++) source[5 + g * 11] = g * 17 + 9;
    const unsigned counts[] = {0, 1, 3, 33};
    const uint32_t firsts[] = {0, 4096, UINT32_MAX - 1};
    unsigned cases = 0;
    for (unsigned n = 0; n < 4; n++) for (unsigned f = 0; f < 3; f++) for (unsigned test = 0; test < 11; test++) {
        uint32_t ctl[8] = {firsts[f], counts[n], 13, 35, 5, 35, 11, 0};
        unsigned in_bound = INPUT_WORDS, out_bound = OUTPUT_WORDS;
        switch (test) {
            case 1: ctl[3] = 0; break;
            case 2: ctl[2] = UINT32_MAX; break;
            case 3: ctl[2] = OUTPUT_WORDS - 1; break;
            case 4: ctl[5] = 0; break;
            case 5: ctl[4] = UINT32_MAX; break;
            case 6: ctl[6] = 0; break;
            case 7: out_bound = 13 + 2 * words - 1; break;
            case 8: in_bound = 5 + 11 + 9 - 1; break;
            case 9: ctl[6] = 8; break;
            case 10: ctl[3] = 2; break;
        }
        unsigned groups = 0;
        // Independently count whole records, with subtraction before addition.
        while (groups < ctl[1] && groups < ctl[3]) {
            uint64_t out_at = (uint64_t)ctl[2] + (uint64_t)groups * words;
            if (out_at > out_bound || words > out_bound - out_at) break;
            if (variant == 3) {
                uint64_t in_at = (uint64_t)ctl[4] + (uint64_t)groups * ctl[6];
                if (groups >= ctl[5] || ctl[6] < 9 || in_at > in_bound || 9 > in_bound - in_at) break;
            }
            groups++;
        }
        memset(output.contents, 0xcc, OUTPUT_WORDS * 4);
        if (groups) {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:input offset:0 atIndex:1];
            [encoder setBuffer:output offset:0 atIndex:2];
            if (variant != 3) [encoder setBytes:&ctl[0] length:sizeof(uint32_t) atIndex:29];
            [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(threads[variant], 1, 1)];
            [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
        }
        if (fwrite(output.contents, sizeof(uint32_t), OUTPUT_WORDS, stdout) != OUTPUT_WORDS) return 1;
        cases++;
    }
    fprintf(stderr, "Metal task/mesh variant%u: %u cases completed\n", variant, cases); return 0;
} }
