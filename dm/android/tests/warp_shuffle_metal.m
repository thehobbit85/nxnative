// Run the Apple translation and check every output lane and the trailing guard words.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

static BOOL check(NSString *directory, NSDictionary *test, id<MTLDevice> device, id<MTLCommandQueue> queue) {
    NSString *stem = [directory stringByAppendingPathComponent:test[@"key"]];
    NSData *expected = [NSData dataWithContentsOfFile:[stem stringByAppendingString:@".expected"]];
    NSError *error = nil;
    NSURL *url = [NSURL fileURLWithPath:[stem stringByAppendingString:@".metallib"]];
    id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
    if (!library || !expected.length) return NO;
    id<MTLFunction> fn = [library newFunctionWithName:@"main0"];
    if (!fn) return NO;
    id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:fn error:&error];
    if (!state) return NO;

    id<MTLArgumentEncoder> args = [fn newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> arguments = [device newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:expected.length options:MTLResourceStorageModeShared];
    if (!arguments || !output) return NO;
    [args setArgumentBuffer:arguments offset:0];
    [args setBuffer:output offset:0 atIndex:[test[@"metal_slot"] unsignedIntegerValue]];
    uint32_t *words = output.contents;
    if (test[@"initial"]) {
        NSData *initial = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:test[@"initial"]]];
        if (initial.length != output.length) return NO;
        memcpy(words, initial.bytes, output.length);
    } else {
        for (size_t i = 0; i < output.length / sizeof(*words); i++) words[i] = 0xbaddecaf;
    }
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:state];
    [enc setBuffer:arguments offset:0 atIndex:0];
    [enc useResource:output usage:(MTLResourceUsageRead | MTLResourceUsageWrite)];
    NSArray *local = test[@"local"];
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake([local[0] unsignedIntegerValue], [local[1] unsignedIntegerValue], [local[2] unsignedIntegerValue])];
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    if (cmd.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "%s\n", cmd.error.description.UTF8String);
        return NO;
    }
    const uint32_t *reference = expected.bytes;
    for (size_t i = 0; i < expected.length / sizeof(*words); i++) {
        if (words[i] != reference[i]) {
            fprintf(stderr, "%s word %zu: got %08x, expected %08x\n",
                    [test[@"key"] UTF8String], i, words[i], reference[i]);
            return NO;
        }
    }
    return YES;
}

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    NSString *directory = @(argv[1]);
    NSData *json = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"cases.json"]];
    NSArray *cases = json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil] : nil;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return 77;
    if (!cases.count) return 1;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    for (NSDictionary *test in cases) { @autoreleasepool {
        if (!check(directory, test, device, queue)) {
            fprintf(stderr, "Metal compute comparison failed for %s\n", [test[@"key"] UTF8String]);
            return 1;
        }
    } }
    printf("Metal verified %lu guest compute cases\n", (unsigned long)cases.count);
    return 0;
} }
