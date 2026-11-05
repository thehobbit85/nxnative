// Execute unmodified Apple-target translations of the texture-operation corpus.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "texture_ops_matrix.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc<3 || argc>4)return 2;unsigned variant=(unsigned)strtoul(argv[2],NULL,10),mode=argc==4?(unsigned)strtoul(argv[3],NULL,10):0;
    if(variant>=TEX_OP_VARIANTS || mode>7 || (mode && variant!=1 && variant!=3 && variant!=9))return 2;
    tex_config_t config=tex_config(mode);
    NSString *directory=@(argv[1]),*key=[NSString stringWithFormat:@"%016llx",0xfeed7000ull+variant];
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    if((config.min_linear || config.mag_linear) && !device.supports32BitFloatFiltering)return 77;
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
        else if([kind isEqualToString:@"sbuf_ubo"]) { [arguments setBuffer:output offset:256 atIndex:index];seen|=2; }
        else if([kind isEqualToString:@"support"]) [arguments setBuffer:support offset:0 atIndex:index];
        else if([kind isEqualToString:@"tex"]) { [arguments setTexture:texture atIndex:index];seen|=4; }
        else if([kind isEqualToString:@"samp"]) { samplerIndex=index;seen|=8; }
        else return 1;
    }
    if((seen&7)!=7)return 1;
    id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 1;
    for(unsigned index=0;index<tex_matrix_count(mode);index++) { @autoreleasepool {
        uint32_t expected[4];tex_matrix_inputs(variant,mode,index,config,uniform.contents,expected);memset(output.contents,0xcc,1024);
        if(seen&8)[arguments setSamplerState:samplers[tex_matrix_wrap(mode,index)] atIndex:samplerIndex];
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if(!command || !encoder)return 1;
        [encoder setComputePipelineState:pipeline];[encoder setBuffer:argumentBuffer offset:0 atIndex:0];
        [encoder useResource:uniform usage:MTLResourceUsageRead];[encoder useResource:output usage:MTLResourceUsageWrite];
        [encoder useResource:support usage:MTLResourceUsageRead];[encoder useResource:texture usage:MTLResourceUsageRead];
        [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
        if(tex_matrix_check(variant,mode,index,output.contents,expected))return 1;
    } }
    fprintf(stderr,"Metal texture variant%u mode%u: %u cases passed\n",variant,mode,tex_matrix_count(mode));return 0;
} }
