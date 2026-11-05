// Run the Apple-target translation using its actual C2 resource lookup helpers.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include "../../gfx/nvn/dmsh_abi.h"
#include "c2_array_cases.h"
#include "c2_reference.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=3)return 2;unsigned variant=(unsigned)strtoul(argv[2],NULL,10);if(variant>1)return 2;
    NSString *directory=@(argv[1]),*key=[NSString stringWithFormat:@"00000000feed400%u",variant];
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]] error:&error];
    if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLFunction> function=[library newFunctionWithName:@"main0"];
    if(!function)return 1;
    id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];
    if(!pipeline) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLArgumentEncoder> arguments=[function newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> argumentBuffer=[device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> uniform=[device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    if(!argumentBuffer || !uniform || !output)return 1;
    [arguments setArgumentBuffer:argumentBuffer offset:0];
    id<MTLTexture> textures[3];id<MTLSamplerState> samplers[4];
    for(unsigned i=0;i<3;i++) {
        MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:1 height:1 mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
        textures[i]=[device newTextureWithDescriptor:descriptor];if(!textures[i])return 1;
        float value=i==1?0.25f:i==2?0.75f:0;[textures[i] replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&value bytesPerRow:4];
    }
    for(unsigned i=0;i<4;i++) {
        MTLSamplerDescriptor *descriptor=[MTLSamplerDescriptor new];descriptor.supportArgumentBuffers=YES;
        descriptor.sAddressMode=i>=2?MTLSamplerAddressModeClampToBorderColor:MTLSamplerAddressModeClampToEdge;
        descriptor.tAddressMode=MTLSamplerAddressModeClampToEdge;descriptor.rAddressMode=MTLSamplerAddressModeClampToEdge;
        descriptor.borderColor=i==3?MTLSamplerBorderColorOpaqueWhite:MTLSamplerBorderColorOpaqueBlack;
        samplers[i]=[device newSamplerStateWithDescriptor:descriptor];if(!samplers[i])return 1;
    }
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".reflection.json"]]];
    NSDictionary *reflection=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];if(!reflection)return 1;
    c2_test_inputs(S.tex[5],S.img[5],S.samp[5]);unsigned seen=0;
    for(NSDictionary *binding in reflection[@"bindings"]) {
        NSString *kind=binding[@"kind"];NSUInteger index=[binding[@"arg_id"] unsignedIntegerValue];
        if([kind isEqualToString:@"cbuf"]) { [arguments setBuffer:uniform offset:0 atIndex:index];seen|=1; }
        else if([kind isEqualToString:@"sbuf_ubo"]) { [arguments setBuffer:output offset:256 atIndex:index];seen|=2; }
        else if([kind isEqualToString:@"tex_c2a"] || [kind isEqualToString:@"samp_c2a"]) {
            unsigned offset=[binding[@"off"] unsignedIntValue];
            uint64_t fixed=binding[@"soff"]?c2_handle(5,[binding[@"soff"] unsignedIntValue]):0;
            for(unsigned i=0;i<C2_TEST_COUNT;i++) {
                uint64_t handle=c2_handle(5,offset+8*i);
                if([kind isEqualToString:@"tex_c2a"]) {
                    unsigned tid=handle&0xfffff;if(tid>2)return 1;[arguments setTexture:textures[tid] atIndex:index+i];seen|=4;
                } else {
                    unsigned sid=(unsigned)((handle|fixed)>>20);if(sid>3)return 1;[arguments setSamplerState:samplers[sid] atIndex:index+i];seen|=8;
                }
            }
        } else return 1;
    }
    if(seen!=15)return 1;
    id<MTLCommandQueue> queue=[device newCommandQueue];unsigned cases=0;
    for(unsigned index=0;index<C2_TEST_INDICES;index++)for(unsigned coord=0;coord<C2_TEST_COORDS;coord++) { @autoreleasepool {
        memset(uniform.contents,0,65536);uint32_t *input=uniform.contents;input[0]=c2_test_offset(index);input[1]=c2_test_coords[coord];memset(output.contents,0xcc,1024);
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];[encoder setBuffer:argumentBuffer offset:0 atIndex:0];
        [encoder useResource:uniform usage:MTLResourceUsageRead];[encoder useResource:output usage:MTLResourceUsageWrite];
        for(unsigned i=0;i<3;i++)[encoder useResource:textures[i] usage:MTLResourceUsageRead];
        [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
        uint32_t *result=output.contents;
        for(unsigned word=0;word<256;word++) {
            uint32_t expected=word==64?c2_test_expected(index,coord,variant):0xcccccccc;
            if(result[word]!=expected) { fprintf(stderr,"Metal C2 variant%u index%u coord%u word%u: %08x expected %08x\n",variant,index,coord,word,result[word],expected);return 1; }
        }
        if(fwrite(result+64,4,1,stdout)!=1)return 1;cases++;
    } }
    fprintf(stderr,"Metal C2 variant%u: %u cases passed\n",variant,cases);return 0;
} }
