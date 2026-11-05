// Use the shared Apple pool_arrays MSL and its resource-ID table layout.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../../gfx/nvn/dmsh_abi.h"
#include "resource_pool_cases.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;NSString *directory=@(argv[1]),*key=@"00000000feed6000";
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]] error:&error];
    if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }id<MTLFunction> function=[library newFunctionWithName:@"main0"];if(!function)return 1;
    id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];if(!pipeline) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLArgumentEncoder> arguments=[function newArgumentEncoderWithBufferIndex:0];id<MTLBuffer> argumentBuffer=[device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> uniform=[device newBufferWithLength:65536 options:MTLResourceStorageModeShared],input=[device newBufferWithLength:1024 options:MTLResourceStorageModeShared],output=[device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    if(!argumentBuffer || !uniform || !input || !output)return 1;[arguments setArgumentBuffer:argumentBuffer offset:0];
    id<MTLTexture> textures[3];id<MTLSamplerState> samplers[4];
    for(unsigned i=0;i<3;i++) {
        MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:1 height:1 mipmapped:NO];descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
        textures[i]=[device newTextureWithDescriptor:descriptor];if(!textures[i])return 1;float value=i==1?0.25f:i==2?0.75f:0;[textures[i] replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&value bytesPerRow:4];
    }
    for(unsigned i=0;i<4;i++) {
        MTLSamplerDescriptor *descriptor=[MTLSamplerDescriptor new];descriptor.supportArgumentBuffers=YES;descriptor.sAddressMode=i>=2?MTLSamplerAddressModeClampToBorderColor:MTLSamplerAddressModeClampToEdge;descriptor.tAddressMode=MTLSamplerAddressModeClampToEdge;descriptor.rAddressMode=MTLSamplerAddressModeClampToEdge;descriptor.borderColor=i==3?MTLSamplerBorderColorOpaqueWhite:MTLSamplerBorderColorOpaqueBlack;
        samplers[i]=[device newSamplerStateWithDescriptor:descriptor];if(!samplers[i])return 1;
    }
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".reflection.json"]]];
    NSDictionary *reflection=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];if(!reflection)return 1;
    id<MTLCommandQueue> queue=[device newCommandQueue];unsigned cases=0;
    for(unsigned config=0;config<RP_CONFIGS;config++) { @autoreleasepool {
        unsigned nt=rp_texture_bounds[config],ns=rp_sampler_bounds[config];
        id<MTLBuffer> texturePool=[device newBufferWithLength:DMSH_POOL_HEAD+8*(nt+1) options:MTLResourceStorageModeShared],samplerPool=[device newBufferWithLength:DMSH_POOL_HEAD+8*(ns+1) options:MTLResourceStorageModeShared];if(!texturePool || !samplerPool)return 1;
        *(uint32_t*)texturePool.contents=nt;*(uint32_t*)samplerPool.contents=ns;
        uint64_t *textureIDs=(uint64_t*)((uint8_t*)texturePool.contents+DMSH_POOL_HEAD),*samplerIDs=(uint64_t*)((uint8_t*)samplerPool.contents+DMSH_POOL_HEAD);
        for(unsigned i=0;i<=nt;i++)textureIDs[i]=textures[rp_texture(i,nt)].gpuResourceID._impl;
        for(unsigned i=0;i<=ns;i++)samplerIDs[i]=samplers[rp_sampler(i,ns)].gpuResourceID._impl;
        unsigned seen=0;
        for(NSDictionary *binding in reflection[@"bindings"]) {
            NSString *kind=binding[@"kind"];NSUInteger index=[binding[@"arg_id"] unsignedIntegerValue];
            if([kind isEqualToString:@"cbuf"]) { [arguments setBuffer:uniform offset:0 atIndex:index];seen|=1; }
            else if([kind isEqualToString:@"sbuf_ubo"] && [binding[@"off"] unsignedIntValue]==0) { [arguments setBuffer:input offset:256 atIndex:index];seen|=2; }
            else if([kind isEqualToString:@"sbuf_ubo"] && [binding[@"off"] unsignedIntValue]==56) { [arguments setBuffer:output offset:256 atIndex:index];seen|=4; }
            else if([kind isEqualToString:@"tex_pool"]) { [arguments setBuffer:texturePool offset:0 atIndex:index];seen|=8; }
            else if([kind isEqualToString:@"samp_pool"]) { [arguments setBuffer:samplerPool offset:0 atIndex:index];seen|=16; }
            else return 1;
        }
        if(seen!=31)return 1;
        for(unsigned test=0;test<RP_INDICES;test++)for(unsigned coord=0;coord<RP_COORDS;coord++) { @autoreleasepool {
            memset(uniform.contents,0,65536);uint32_t *u=uniform.contents;u[0]=0x10000;u[14]=0x20000;u[4]=rp_coords[coord];memset(input.contents,0xcc,1024);memset(output.contents,0xcc,1024);
            uint32_t *handles=input.contents;for(unsigned lane=0;lane<RP_LANES;lane++)handles[64+lane]=rp_handle(config,test,lane);
            id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];[encoder setComputePipelineState:pipeline];[encoder setBuffer:argumentBuffer offset:0 atIndex:0];
            [encoder useResource:uniform usage:MTLResourceUsageRead];[encoder useResource:input usage:MTLResourceUsageRead];[encoder useResource:output usage:MTLResourceUsageWrite];[encoder useResource:texturePool usage:MTLResourceUsageRead];[encoder useResource:samplerPool usage:MTLResourceUsageRead];for(unsigned i=0;i<3;i++)[encoder useResource:textures[i] usage:MTLResourceUsageRead];
            [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(RP_LANES,1,1)];[encoder endEncoding];[command commit];[command waitUntilCompleted];if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
            uint32_t *result=output.contents;
            for(unsigned word=0;word<256;word++) { uint32_t expected=word>=64 && word<72?rp_expected(config,test,coord,word-64):0xcccccccc;if(result[word]!=expected) { fprintf(stderr,"Metal pool config%u index%u coord%u word%u: %08x expected %08x\n",config,test,coord,word,result[word],expected);return 1; } }
            if(fwrite(result+64,4,RP_LANES,stdout)!=RP_LANES)return 1;cases+=RP_LANES;
        } }
    } }
    fprintf(stderr,"Metal resource pools: %u samples passed\n",cases);return 0;
} }
