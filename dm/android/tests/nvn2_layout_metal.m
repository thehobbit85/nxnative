// Run the shared Ampere translation against distinct bytes in all mapped banks.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nvn2_layout_cases.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;NSString *directory=@(argv[1]),*key=@"00000000feed9100";NSError *error=nil;
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]] error:&error];
    if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }id<MTLFunction> function=[library newFunctionWithName:@"main0"];if(!function)return 1;
    id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];if(!pipeline) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLArgumentEncoder> encoder=[function newArgumentEncoderWithBufferIndex:0];if(!encoder)return 1;
    id<MTLBuffer> argumentBuffer=[device newBufferWithLength:encoder.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> input=[device newBufferWithLength:8448 options:MTLResourceStorageModeShared],output=[device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    if(!argumentBuffer||!input||!output)return 1;[encoder setArgumentBuffer:argumentBuffer offset:0];
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".reflection.json"]]];
    if(!data)return 1;
    NSDictionary *reflection=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];if(!reflection)return 1;
    unsigned seen=0;
    for(NSDictionary *binding in reflection[@"bindings"]) {
        NSString *kind=binding[@"kind"];NSUInteger index=[binding[@"arg_id"] unsignedIntegerValue];int reg=[binding[@"reg"] intValue];
        if([kind isEqualToString:@"sbuf_ubo"]&&reg==0&&[binding[@"off"] intValue]==56) { [encoder setBuffer:output offset:256 atIndex:index];seen|=8; }
        else if([kind isEqualToString:@"drvcb"]) {
            unsigned bank=[binding[@"bank"] unsignedIntValue];if(bank!=0&&bank!=2&&bank!=24)return 1;
            unsigned part=bank==0?0:bank==2?1:2;[encoder setBuffer:input offset:part*4096 atIndex:index];seen|=1u<<part;
        } else return 1;
    }
    if(seen!=15)return 1;id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 1;
    for(unsigned test=0;test<NL_CASES;test++) { @autoreleasepool {
        uint32_t *words=input.contents;
        for(unsigned i=0;i<1024;i++) { words[i]=nl_word(0,i*4,test);words[1024+i]=nl_word(2,i*4,test); }
        for(unsigned i=0;i<64;i++)words[2048+i]=nl_word(24,i*4,test);memset(output.contents,0xcc,1024);
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> compute=[command computeCommandEncoder];if(!command||!compute)return 1;
        [compute setComputePipelineState:pipeline];[compute setBuffer:argumentBuffer offset:0 atIndex:0];
        [compute useResource:input usage:MTLResourceUsageRead];[compute useResource:output usage:MTLResourceUsageWrite];
        [compute dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[compute endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
        if(nl_check(test,output.contents))return 1;
    } }
    fprintf(stderr,"Metal NVN2 layout:16 cases passed\n");return 0;
} }
