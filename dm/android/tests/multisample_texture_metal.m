// Read GPU-seeded per-sample values through unmodified Apple translations.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "multisample_texture_cases.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=3)return 2;unsigned variant=(unsigned)strtoul(argv[2],NULL,10);if(variant>3)return 2;unsigned array=variant>=2,layers=array?3:1;
    NSString *directory=@(argv[1]),*key=[NSString stringWithFormat:@"%016llx",0xfeed9800ull+variant];
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]] error:&error];
    if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLFunction> function=[library newFunctionWithName:@"main0"];if(!function)return 1;
    id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];
    if(!pipeline) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLArgumentEncoder> arguments=[function newArgumentEncoderWithBufferIndex:0];if(!arguments)return 1;
    id<MTLBuffer> argumentBuffer=[device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> uniform=[device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    id<MTLBuffer> support=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    if(!argumentBuffer || !uniform || !output || !support)return 1;
    memset(support.contents,0,4096);for(unsigned i=0;i<73*4;i++)((float*)support.contents)[48+i]=1;
    [arguments setArgumentBuffer:argumentBuffer offset:0];
    MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:MS_SIDE height:MS_SIDE mipmapped:NO];
    descriptor.textureType=array?MTLTextureType2DMultisampleArray:MTLTextureType2DMultisample;descriptor.arrayLength=layers;descriptor.sampleCount=MS_SAMPLES;
    descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView|MTLTextureUsageRenderTarget;
    id<MTLTexture> storage=[device newTextureWithDescriptor:descriptor];if(!storage)return 1;
    NSString *seedSource=@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 seedVertex(uint id [[vertex_id]]){return float4(id==1?3:-1,id==2?3:-1,0,1);}\nfragment float4 seedFragment(float4 pos [[position]],uint sample [[sample_id]],constant uint &layer [[buffer(0)]]){float v=float(10000*layer+1000*sample+100*uint(pos.y)+uint(pos.x)+1);return float4(v,v+100000,v+200000,v+300000);}";
    id<MTLLibrary> seedLibrary=[device newLibraryWithSource:seedSource options:nil error:&error];if(!seedLibrary){fprintf(stderr,"%s\n",error.description.UTF8String);return 1;}
    MTLRenderPipelineDescriptor *seedDescriptor=[MTLRenderPipelineDescriptor new];seedDescriptor.vertexFunction=[seedLibrary newFunctionWithName:@"seedVertex"];seedDescriptor.fragmentFunction=[seedLibrary newFunctionWithName:@"seedFragment"];seedDescriptor.rasterSampleCount=MS_SAMPLES;seedDescriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> seedPipeline=[device newRenderPipelineStateWithDescriptor:seedDescriptor error:&error];if(!seedPipeline){fprintf(stderr,"%s\n",error.description.UTF8String);return 1;}
    id<MTLCommandQueue> seedQueue=[device newCommandQueue];id<MTLCommandBuffer> seedCommand=[seedQueue commandBuffer];if(!seedCommand)return 1;
    for(uint32_t layer=0;layer<layers;layer++) {
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=storage;pass.colorAttachments[0].slice=layer;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(-1,-1,-1,-1);
        id<MTLRenderCommandEncoder> encoder=[seedCommand renderCommandEncoderWithDescriptor:pass];if(!encoder)return 1;[encoder setRenderPipelineState:seedPipeline];[encoder setFragmentBytes:&layer length:4 atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
    }
    [seedCommand commit];[seedCommand waitUntilCompleted];if(seedCommand.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"%s\n",seedCommand.error.description.UTF8String);return 1;}
    id<MTLTexture> texture=[storage newTextureViewWithPixelFormat:MTLPixelFormatRGBA32Float textureType:descriptor.textureType levels:NSMakeRange(0,1) slices:NSMakeRange(array?1:0,array?2:1)];
    if(!texture)return 1;
    id<MTLSamplerState> samplers[2];
    for(unsigned i=0;i<2;i++) {
        MTLSamplerDescriptor *sampler=[MTLSamplerDescriptor new];sampler.supportArgumentBuffers=YES;
        sampler.sAddressMode=sampler.tAddressMode=sampler.rAddressMode=i?MTLSamplerAddressModeRepeat:MTLSamplerAddressModeClampToEdge;
        sampler.minFilter=MTLSamplerMinMagFilterNearest;
        sampler.magFilter=MTLSamplerMinMagFilterNearest;
        sampler.mipFilter=MTLSamplerMipFilterNearest;
        sampler.lodMinClamp=0;sampler.lodMaxClamp=3;
        samplers[i]=[device newSamplerStateWithDescriptor:sampler];if(!samplers[i])return 1;
    }
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".reflection.json"]]];if(!data)return 1;
    NSDictionary *reflection=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];if(!reflection)return 1;
    unsigned seen=0;NSUInteger samplerIndex=0;
    for(NSDictionary *binding in reflection[@"bindings"]) {
        NSString *kind=binding[@"kind"];NSUInteger index=[binding[@"arg_id"] unsignedIntegerValue];
        if([kind isEqualToString:@"cbuf"]) { [arguments setBuffer:uniform offset:0 atIndex:index];seen|=1; }
        else if([kind isEqualToString:@"sbuf_ubo"]) { [arguments setBuffer:output offset:256 atIndex:index];seen|=2; }
        else if([kind isEqualToString:@"support"]) [arguments setBuffer:support offset:0 atIndex:index];
        else if([kind isEqualToString:@"tex"]) { [arguments setTexture:texture atIndex:index];seen|=4; }
        else if([kind isEqualToString:@"samp"]) { samplerIndex=index;seen|=8; }
        else return 1;
    }
    if((seen&6)!=6)return 1;
    id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 1;
    for(unsigned index=0;index<ms_count(variant);index++) { @autoreleasepool {
        uint32_t expected[4];ms_inputs(variant,index,uniform.contents,expected);if(!(variant%2) && getenv("MS_WRONG_SAMPLE"))((uint32_t*)uniform.contents)[3]=(((uint32_t*)uniform.contents)[3]+1)%MS_SAMPLES;memset(output.contents,0xcc,1024);
        if(seen&8)[arguments setSamplerState:samplers[0] atIndex:samplerIndex];
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if(!command || !encoder)return 1;
        [encoder setComputePipelineState:pipeline];[encoder setBuffer:argumentBuffer offset:0 atIndex:0];
        [encoder useResource:uniform usage:MTLResourceUsageRead];[encoder useResource:output usage:MTLResourceUsageWrite];
        [encoder useResource:support usage:MTLResourceUsageRead];[encoder useResource:texture usage:MTLResourceUsageRead];
        [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
        if(ms_check(variant,index,output.contents,expected))return 1;
    } }
    fprintf(stderr,"Metal multisample variant%u: %u cases passed\n",variant,ms_count(variant));return 0;
} }
