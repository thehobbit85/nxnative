// Read the shared Apple reflection and C1 payload, then compare exact load bits.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "constant_banks_cases.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=3)return 2;unsigned variant=(unsigned)strtoul(argv[2],NULL,10);if(variant>=CB_VARIANTS)return 2;
    unsigned bank=cb_bank(variant),width=variant%6;NSString *directory=@(argv[1]),*key=[NSString stringWithFormat:@"%016llx",0xfeed9000ull+variant];NSError *error=nil;
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]] error:&error];
    if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }id<MTLFunction> function=[library newFunctionWithName:@"main0"];if(!function)return 1;
    id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];if(!pipeline) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLArgumentEncoder> encoder=[function newArgumentEncoderWithBufferIndex:0];if(!encoder)return 1;
    id<MTLBuffer> argumentBuffer=[device newBufferWithLength:encoder.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> uniform=[device newBufferWithLength:65536 options:MTLResourceStorageModeShared],input=[device newBufferWithLength:65536 options:MTLResourceStorageModeShared],output=[device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    if(!argumentBuffer||!uniform||!input||!output)return 1;[encoder setArgumentBuffer:argumentBuffer offset:0];
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".reflection.json"]]];
    if(!data)return 1;
    NSDictionary *reflection=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];if(!reflection)return 1;
    unsigned seen=0;
    for(NSDictionary *binding in reflection[@"bindings"]) {
        NSString *kind=binding[@"kind"];NSUInteger index=[binding[@"arg_id"] unsignedIntegerValue];int reg=[binding[@"reg"] intValue];
        if([kind isEqualToString:@"cbuf"]&&reg==0) { [encoder setBuffer:uniform offset:0 atIndex:index];seen|=1; }
        else if([kind isEqualToString:@"sbuf_ubo"]&&reg==0&&[binding[@"off"] intValue]==56) { [encoder setBuffer:output offset:256 atIndex:index];seen|=2; }
        else if(([kind isEqualToString:@"cbuf"]&&(reg==1||reg==13))||[kind isEqualToString:@"c1"]||[kind isEqualToString:@"drvcb"]) {
            unsigned source=[kind isEqualToString:@"cbuf"]?(unsigned)(reg+3):[kind isEqualToString:@"c1"]?1:[binding[@"bank"] unsignedIntValue];
            for(unsigned i=0;i<65536;i++)((uint8_t*)input.contents)[i]=cb_pattern(source,i);
            if(source==1) { memset(input.contents,0,65536);if(cb_decode([reflection[@"c1"] UTF8String],input.contents,256))return 1; }
            [encoder setBuffer:input offset:0 atIndex:index];seen|=4;
        } else return 1;
    }
    if(seen!=7)return 1;id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 1;
    for(unsigned test=0;test<CB_CASES;test++) { @autoreleasepool {
        memset(uniform.contents,0,65536);((uint32_t*)uniform.contents)[0]=cb_offset(bank,width,test);((uint32_t*)uniform.contents)[14]=0x1000;memset(output.contents,0xcc,1024);
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> compute=[command computeCommandEncoder];if(!command||!compute)return 1;
        [compute setComputePipelineState:pipeline];[compute setBuffer:argumentBuffer offset:0 atIndex:0];
        [compute useResource:uniform usage:MTLResourceUsageRead];[compute useResource:input usage:MTLResourceUsageRead];[compute useResource:output usage:MTLResourceUsageWrite];
        [compute dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[compute endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
        if(cb_check(variant,test,output.contents))return 1;
    } }
    fprintf(stderr,"Metal constants bank%u width%u:40 cases passed\n",bank,width);return 0;
} }
