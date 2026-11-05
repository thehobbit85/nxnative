// Execute unmodified Apple-target translations of the texture-operation corpus.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "texture_compare_cases.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc<3 || argc>4)return 2;unsigned variant=(unsigned)strtoul(argv[2],NULL,10),mode=argc==4?(unsigned)strtoul(argv[3],NULL,10):0;
    if(variant>3 || mode>1)return 2;unsigned array=variant/2;
    NSString *directory=@(argv[1]),*key=[NSString stringWithFormat:@"%016llx",0xfeed9400ull+variant];
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
    MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:8 height:8 mipmapped:YES];
    descriptor.textureType=array?MTLTextureType2DArray:MTLTextureType2D;descriptor.arrayLength=array?4:1;
    descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
    id<MTLTexture> storage=[device newTextureWithDescriptor:descriptor];if(!storage)return 1;
    id<MTLBuffer> staging=[device newBufferWithLength:16384 options:MTLResourceStorageModeShared];if(!staging)return 1;
    id<MTLCommandQueue> uploadQueue=[device newCommandQueue];id<MTLCommandBuffer> upload=[uploadQueue commandBuffer];id<MTLBlitCommandEncoder> blit=[upload blitCommandEncoder];if(!blit)return 1;
    unsigned offset=0;
    for(unsigned mip=0;mip<4;mip++) {
        unsigned size=tc_size(mip);
        for(unsigned layer=0;layer<(array?4u:1u);layer++) {
            for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++)((float*)((uint8_t*)staging.contents+offset+y*256))[x]=tc_depth(mip,layer,x,y);
            [blit copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:256 sourceBytesPerImage:256*size sourceSize:MTLSizeMake(size,size,1) toTexture:storage destinationSlice:layer destinationLevel:mip destinationOrigin:MTLOriginMake(0,0,0)];
            offset+=size*256;
        }
    }
    [blit endEncoding];[upload commit];[upload waitUntilCompleted];if(upload.status!=MTLCommandBufferStatusCompleted)return 1;
    id<MTLTexture> texture=[storage newTextureViewWithPixelFormat:MTLPixelFormatDepth32Float textureType:descriptor.textureType levels:NSMakeRange(0,4) slices:NSMakeRange(array?1:0,array?3:1)];
    if(!texture)return 1;
    id<MTLSamplerState> samplers[2];
    for(unsigned i=0;i<2;i++) {
        MTLSamplerDescriptor *sampler=[MTLSamplerDescriptor new];sampler.supportArgumentBuffers=YES;
        sampler.sAddressMode=sampler.tAddressMode=sampler.rAddressMode=i?MTLSamplerAddressModeRepeat:MTLSamplerAddressModeClampToEdge;
        sampler.compareFunction=MTLCompareFunctionLessEqual;sampler.minFilter=mode?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
        sampler.magFilter=mode?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
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
    if((seen&7)!=7)return 1;
    id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 1;
    for(unsigned index=0;index<TC_CASES;index++) { @autoreleasepool {
        uint32_t expected[4];tc_inputs(variant,mode,index,uniform.contents,expected);if(getenv("TC_WRONG_REFERENCE"))((uint32_t*)uniform.contents)[5]=tc_bits(1.125f);memset(output.contents,0xcc,1024);
        if(seen&8)[arguments setSamplerState:samplers[tc_wrap(index)] atIndex:samplerIndex];
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if(!command || !encoder)return 1;
        [encoder setComputePipelineState:pipeline];[encoder setBuffer:argumentBuffer offset:0 atIndex:0];
        [encoder useResource:uniform usage:MTLResourceUsageRead];[encoder useResource:output usage:MTLResourceUsageWrite];
        [encoder useResource:support usage:MTLResourceUsageRead];[encoder useResource:texture usage:MTLResourceUsageRead];
        [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
        if(tc_check(variant,mode,index,output.contents,expected))return 1;
    } }
    fprintf(stderr,"Metal depth compare variant%u linear%u: %u cases passed\n",variant,mode,TC_CASES);return 0;
} }
