// Rasterize unmodified Apple translations of Maxwell LOD queries.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "fragment_lod_query_cases.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc<3 || argc>4)return 2;unsigned variant=(unsigned)strtoul(argv[2],NULL,10),mode=argc==4?(unsigned)strtoul(argv[3],NULL,10):0;
    if(variant>1 || mode>6)return 2;
    tex_config_t config=lod_config(mode);
    NSString *directory=@(argv[1]),*key=[NSString stringWithFormat:@"%016llx",0xfeed9700ull+variant];
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    if((config.min_linear || config.mag_linear) && !device.supports32BitFloatFiltering)return 77;
    NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]] error:&error];
    if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLFunction> function=[library newFunctionWithName:@"main0"];if(!function)return 1;
    NSString *vs=@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 fullscreen(uint id [[vertex_id]]){return float4(id==1?3:-1,id==2?3:-1,0,1);}";
    id<MTLLibrary> vertexLibrary=[device newLibraryWithSource:vs options:nil error:&error];if(!vertexLibrary){fprintf(stderr,"%s\n",error.description.UTF8String);return 1;}
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[vertexLibrary newFunctionWithName:@"fullscreen"];pd.fragmentFunction=function;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];if(!pipeline){fprintf(stderr,"%s\n",error.description.UTF8String);return 1;}
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:SIDE height:SIDE mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> target=[device newTextureWithDescriptor:td];if(!target)return 1;
    id<MTLArgumentEncoder> arguments=[function newArgumentEncoderWithBufferIndex:0];if(!arguments)return 1;
    id<MTLBuffer> argumentBuffer=[device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> uniform=[device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> support=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    if(!argumentBuffer || !uniform || !output || !support)return 1;
    memset(support.contents,0,4096);for(unsigned i=0;i<73*4;i++)((float*)support.contents)[48+i]=1;
    [arguments setArgumentBuffer:argumentBuffer offset:0];
    MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:config.width height:config.height mipmapped:YES];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> storage=[device newTextureWithDescriptor:descriptor];if(!storage)return 1;
    for(unsigned mip=0;mip<config.levels;mip++) {
        float pixels[16*8*4];tex_matrix_level(pixels,mode,mip,config);
        unsigned width=tex_dimension(config.width,mip),height=tex_dimension(config.height,mip);
        [storage replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:mip withBytes:pixels bytesPerRow:width*16];
    }
    id<MTLTexture> texture=config.base?[storage newTextureViewWithPixelFormat:MTLPixelFormatRGBA32Float textureType:MTLTextureType2D levels:NSMakeRange(config.base,config.levels-config.base) slices:NSMakeRange(0,1)]:storage;
    if(!texture)return 1;
    id<MTLSamplerState> samplers[2];
    for(unsigned i=0;i<2;i++) {
        MTLSamplerDescriptor *sampler=[MTLSamplerDescriptor new];sampler.supportArgumentBuffers=YES;
        sampler.sAddressMode=sampler.tAddressMode=i?MTLSamplerAddressModeRepeat:MTLSamplerAddressModeClampToEdge;
        sampler.minFilter=config.min_linear?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
        sampler.magFilter=config.mag_linear?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
        sampler.mipFilter=config.mip_linear?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNearest;
        sampler.lodMinClamp=config.min_lod;sampler.lodMaxClamp=config.max_lod;
        samplers[i]=[device newSamplerStateWithDescriptor:sampler];if(!samplers[i])return 1;
    }
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".reflection.json"]]];if(!data)return 1;
    NSDictionary *reflection=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];if(!reflection)return 1;
    unsigned seen=0;NSUInteger samplerIndex=0;
    for(NSDictionary *binding in reflection[@"bindings"]) {
        NSString *kind=binding[@"kind"];NSUInteger index=[binding[@"arg_id"] unsignedIntegerValue];
        if([kind isEqualToString:@"cbuf"]) { [arguments setBuffer:uniform offset:0 atIndex:index];seen|=1; }
        else if([kind isEqualToString:@"support"]) { [arguments setBuffer:support offset:0 atIndex:index];seen|=2; }
        else if([kind isEqualToString:@"tex"]) { [arguments setTexture:texture atIndex:index];seen|=4; }
        else if([kind isEqualToString:@"samp"]) { samplerIndex=index;seen|=8; }
        else return 1;
    }
    if(seen!=15)return 1;
    id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 1;
    for(unsigned index=0;index<FRAGMENT_CASES;index++) { @autoreleasepool {
        fragment_inputs(mode,index,uniform.contents);memset(output.contents,0xcc,4096);
        if(seen&8)[arguments setSamplerState:samplers[index%2] atIndex:samplerIndex];
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(-999,-999,-999,-999);
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];if(!command||!encoder)return 1;
        [encoder setRenderPipelineState:pipeline];[encoder setFragmentBuffer:argumentBuffer offset:0 atIndex:0];
        [encoder useResource:uniform usage:MTLResourceUsageRead stages:MTLRenderStageFragment];[encoder useResource:support usage:MTLResourceUsageRead stages:MTLRenderStageFragment];[encoder useResource:texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];[command commit];[command waitUntilCompleted];if(command.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1;}
        [target getBytes:output.contents bytesPerRow:SIDE*16 fromRegion:MTLRegionMake2D(0,0,SIDE,SIDE) mipmapLevel:0];
        if(fragment_check(variant,mode,index,output.contents))return 1;
    } }
    fprintf(stderr,"Metal fragment LOD query variant%u mode%u: %u cases passed\n",variant,mode,FRAGMENT_CASES);return 0;
} }
