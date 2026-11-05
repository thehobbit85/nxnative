// Execute the Apple target's unmodified physical-pointer shader translation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "global_memory_cases.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;NSString *directory=@(argv[1]);
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    id<MTLBuffer> buffers[GM_BUFFERS];void *mapped[GM_BUFFERS];
    for(unsigned i=0;i<GM_BUFFERS;i++) { buffers[i]=[device newBufferWithLength:gm_sizes[i] options:MTLResourceStorageModeShared];if(!buffers[i])return 1;mapped[i]=buffers[i].contents; }
    id<MTLCommandQueue> queue=[device newCommandQueue];unsigned cases=0;
    for(unsigned variant=0;variant<GM_VARIANTS;variant++) { @autoreleasepool {
        NSString *key=[NSString stringWithFormat:@"%016llx",0xfeed5000ull+variant];NSError *error=nil;
        id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]] error:&error];
        if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
        id<MTLFunction> function=[library newFunctionWithName:@"main0"];if(!function)return 1;
        id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];
        if(!pipeline) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
        id<MTLArgumentEncoder> arguments=[function newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> argumentBuffer=[device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];if(!argumentBuffer)return 1;
        [arguments setArgumentBuffer:argumentBuffer offset:0];
        NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".reflection.json"]]];
        NSDictionary *reflection=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];if(!reflection)return 1;
        unsigned seen=0;
        for(NSDictionary *binding in reflection[@"bindings"]) {
            NSString *kind=binding[@"kind"];unsigned index=GM_BUFFERS;
            if([kind isEqualToString:@"cbuf"])index=GM_UNIFORM;
            else if([kind isEqualToString:@"sbuf_ubo"])index=[binding[@"off"] unsignedIntValue]==0?GM_POINTER:GM_OUTPUT;
            else if([kind isEqualToString:@"gmem_map"])index=GM_MAP;
            else if([kind isEqualToString:@"gmem_log"])index=GM_LOG;
            if(index==GM_BUFFERS || (seen&(1u<<index)))return 1;
            seen|=1u<<index;[arguments setBuffer:buffers[index] offset:0 atIndex:[binding[@"arg_id"] unsignedIntegerValue]];
        }
        unsigned expected=(1u<<GM_POINTER)|(1u<<GM_MAP)|(1u<<GM_OUTPUT);if(variant>=7)expected|=(1u<<GM_UNIFORM)|(1u<<GM_LOG);if(seen!=expected)return 1;
        for(unsigned ci=0;ci<4;ci++)for(unsigned test=0;test<GM_ADDRESSES;test++)for(unsigned mode=0;mode<3;mode++) { @autoreleasepool {
            unsigned pools=gm_counts[ci];if(gm_address(pools,test)%gm_alignment(variant))continue;
            gm_prepare(mapped,buffers[GM_POOL].gpuAddress,variant,pools,test,mode);uint8_t want_pool[1024];uint32_t want_log[1088],want_output[64];gm_expected(mapped,variant,pools,test,want_pool,want_log,want_output);
            id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];[encoder setBuffer:argumentBuffer offset:0 atIndex:0];
            for(unsigned i=0;i<GM_BUFFERS;i++)[encoder useResource:buffers[i] usage:MTLResourceUsageRead|MTLResourceUsageWrite];
            [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[encoder endEncoding];[command commit];[command waitUntilCompleted];
            if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
            if(gm_check(mapped,variant,pools,test,mode,want_pool,want_log,want_output))return 1;cases++;
        } }
    } }
    fprintf(stderr,"Metal global memory: %u cases passed\n",cases);return 0;
} }
