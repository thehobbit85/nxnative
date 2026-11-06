// Direct Apple conversion control: no texture or guest instruction can alter inputs.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;
    NSData *input=[NSData dataWithContentsOfFile:@(argv[1])];
    if(!input.length || input.length>32*1024*1024 || input.length%16)return 2;
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\nkernel void half_pack(device uint4 *data [[buffer(0)]],constant uint &count [[buffer(1)]],uint i [[thread_position_in_grid]]) { if(i<count) data[i].z=as_type<uint>(half2(as_type<float2>(data[i].xy))); }";
    MTLCompileOptions *options=[MTLCompileOptions new];
    id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&error];
    if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLFunction> function=[library newFunctionWithName:@"half_pack"];if(!function)return 1;
    id<MTLComputePipelineState> state=[device newComputePipelineStateWithFunction:function error:&error];
    if(!state) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLBuffer> buffer=[device newBufferWithBytes:input.bytes length:input.length options:MTLResourceStorageModeShared];if(!buffer)return 1;
    uint32_t count=(uint32_t)(input.length/16);
    id<MTLCommandQueue> queue=[device newCommandQueue];id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if(!encoder)return 1;
    [encoder setComputePipelineState:state];[encoder setBuffer:buffer offset:0 atIndex:0];[encoder setBytes:&count length:sizeof count atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake((count+63)/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
    return fwrite(buffer.contents,1,input.length,stdout)==input.length?0:1;
} }
