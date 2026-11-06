// Compare the translated guest fragment's source and destination pixel centers.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static BOOL check(NSString *directory, NSDictionary *test, id<MTLDevice> device,
                  id<MTLCommandQueue> queue, id<MTLFunction> vertex) {
    NSString *stem = [directory stringByAppendingPathComponent:test[@"name"]];
    NSData *positions = [NSData dataWithContentsOfFile:[stem stringByAppendingString:@".vertices"]];
    NSData *expected = [NSData dataWithContentsOfFile:[stem stringByAppendingString:@".expected"]];
    NSUInteger side = [test[@"side"] unsignedIntegerValue];
    if (!positions.length || positions.length % 24 || !side || expected.length != side * side * 16) return NO;
    NSString *key = test[@"key"];
    NSURL *url = [NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
    id<MTLFunction> fragment = [library newFunctionWithName:@"main0"];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vertex;
    pd.fragmentFunction = fragment;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!fragment || !pipeline) { fprintf(stderr, "%s\n", error.description.UTF8String); return NO; }

    id<MTLArgumentEncoder> args = [fragment newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> arguments = [device newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> support = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    if (!args || !arguments || !support) return NO;
    for (NSUInteger i = 0; i < support.length / sizeof(float); i++) ((float *)support.contents)[i] = 1;
    [args setArgumentBuffer:arguments offset:0];
    [args setBuffer:support offset:0 atIndex:[test[@"metal_slot"] unsignedIntegerValue]];

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                 width:side height:side mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    if (!target) return NO;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
    if (!enc) return NO;
    [enc setRenderPipelineState:pipeline];
    [enc setVertexBytes:positions.bytes length:positions.length atIndex:0];
    [enc setFragmentBuffer:arguments offset:0 atIndex:0];
    [enc useResource:support usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:positions.length / 8];
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    if (cmd.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "%s\n", cmd.error.description.UTF8String); return NO; }
    NSMutableData *actual = [NSMutableData dataWithLength:expected.length];
    [target getBytes:actual.mutableBytes bytesPerRow:side * 16 fromRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0];
    for (NSUInteger i = 0; i < expected.length / sizeof(float); i++) {
        float value = ((const float *)actual.bytes)[i], want = ((const float *)expected.bytes)[i];
        if (value != want) {
            fprintf(stderr, "%s pixel(%lu,%lu)[%lu] got %g, expected %g\n", [test[@"name"] UTF8String],
                    i / 4 % side, i / 4 / side, i % 4, value, want);
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
    NSError *error = nil;
    NSString *vs = @"#include <metal_stdlib>\nusing namespace metal;\n"
        @"vertex float4 screen(uint id [[vertex_id]], constant float2 *p [[buffer(0)]]){return float4(p[id].x,-p[id].y,0,1);}";
    id<MTLLibrary> lib = [device newLibraryWithSource:vs options:nil error:&error];
    id<MTLFunction> vertex = [lib newFunctionWithName:@"screen"];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!cases.count || !vertex || !queue) return 1;
    for (NSDictionary *test in cases) { @autoreleasepool {
        if (!check(directory, test, device, queue, vertex)) return 1;
    } }
    printf("Metal verified %lu guest fragment draws\n", (unsigned long)cases.count);
    return 0;
} }
