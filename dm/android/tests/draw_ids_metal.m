// Execute unmodified shared Maxwell vertex/instance IDs and C0 reads.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "draw_ids_cases.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;NSString *directory=@(argv[1]),*key=@"00000000feed9200";NSError *error=nil;
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]] error:&error];
    if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }id<MTLFunction> function=[library newFunctionWithName:@"main0"];if(!function)return 1;
    id<MTLLibrary> fragmentLibrary=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;fragment float4 color(){return float4(0);}" options:nil error:&error];if(!fragmentLibrary)return 1;
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=function;pd.fragmentFunction=[fragmentLibrary newFunctionWithName:@"color"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];if(!pipeline) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLArgumentEncoder> encoder=[function newArgumentEncoderWithBufferIndex:0];if(!encoder)return 1;
    id<MTLBuffer> argumentBuffer=[device newBufferWithLength:encoder.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> input=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared],output=[device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    if(!argumentBuffer||!input||!output)return 1;[encoder setArgumentBuffer:argumentBuffer offset:0];
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".reflection.json"]]];
    if(!data)return 1;
    NSDictionary *reflection=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];if(!reflection)return 1;
    unsigned seen=0;
    for(NSDictionary *binding in reflection[@"bindings"]) {
        NSString *kind=binding[@"kind"];NSUInteger index=[binding[@"arg_id"] unsignedIntegerValue];int reg=[binding[@"reg"] intValue];
        if([kind isEqualToString:@"sbuf_ubo"]&&reg==0&&[binding[@"off"] intValue]==56) { [encoder setBuffer:output offset:256 atIndex:index];seen|=2; }
        else if([kind isEqualToString:@"drvcb"]) {
            if([binding[@"bank"] unsignedIntValue]!=0)return 1;
            [encoder setBuffer:input offset:0 atIndex:index];seen|=1;
        } else return 1;
    }
    if(seen!=3)return 1;id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 1;
    MTLTextureDescriptor *texture=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];texture.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> target=[device newTextureWithDescriptor:texture];if(!target)return 1;
    uint16_t indices[]={9,24};id<MTLBuffer> indexBuffer=[device newBufferWithBytes:indices length:sizeof indices options:MTLResourceStorageModeShared];if(!indexBuffer)return 1;
    for(unsigned test=0;test<DI_CASES;test++) { @autoreleasepool {
        memset(input.contents,0,4096);((uint32_t*)input.contents)[0x644/4]=di_base(test);((float*)input.contents)[0x610/4]=2047;
        if(getenv("DI_WRONG_BASE"))((uint32_t*)input.contents)[0x644/4]++;
        memset(output.contents,0xcc,1024);
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.renderTargetWidth=1;pass.renderTargetHeight=1;pass.defaultRasterSampleCount=1;pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionDontCare;
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> render=[command renderCommandEncoderWithDescriptor:pass];if(!command||!render)return 1;
        [render setRenderPipelineState:pipeline];[render setVertexBuffer:argumentBuffer offset:0 atIndex:0];
        [render useResource:input usage:MTLResourceUsageRead stages:MTLRenderStageVertex];[render useResource:output usage:MTLResourceUsageWrite stages:MTLRenderStageVertex];
        if(di_indexed(test)) [render drawIndexedPrimitives:MTLPrimitiveTypePoint indexCount:1 indexType:MTLIndexTypeUInt16 indexBuffer:indexBuffer indexBufferOffset:di_index_offset(test)*2 instanceCount:di_instances(test) baseVertex:di_vertex_base(test) baseInstance:di_base(test)];
        else [render drawPrimitives:MTLPrimitiveTypePoint vertexStart:di_first(test) vertexCount:1 instanceCount:di_instances(test) baseInstance:di_base(test)];
        [render endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
        if(di_check(test,output.contents))return 1;
    } }
    fprintf(stderr,"Metal guest draw IDs:32 cases passed\n");return 0;
} }
