// Read the Apple shader back through its actual reflected texture and support slots.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;NSString *directory=@(argv[1]);NSError *error=nil;
    NSData *json=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"cases.json"]];if(!json)return 1;
    NSArray *rows=[NSJSONSerialization JSONObjectWithData:json options:0 error:&error];if(!rows)return 1;
    NSDictionary *formats=@{@"R32_SFLOAT":@(MTLPixelFormatR32Float),@"R32_UINT":@(MTLPixelFormatR32Uint),
        @"R32G32_UINT":@(MTLPixelFormatRG32Uint),@"R32G32B32A32_UINT":@(MTLPixelFormatRGBA32Uint),
        @"R8_SINT":@(MTLPixelFormatR8Sint),@"R8_UINT":@(MTLPixelFormatR8Uint),@"R16_SINT":@(MTLPixelFormatR16Sint),@"R16_UINT":@(MTLPixelFormatR16Uint)};
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 1;
    for(NSDictionary *row in rows) { @autoreleasepool {
        NSString *path=[directory stringByAppendingPathComponent:row[@"key"]];
        NSData *initial=[NSData dataWithContentsOfFile:[path stringByAppendingString:@".initial"]];
        NSData *expected=[NSData dataWithContentsOfFile:[path stringByAppendingString:@".expected"]];
        if(!initial.length || expected.length!=initial.length || initial.length%32)return 1;
        id<MTLLibrary> lib=[device newLibraryWithURL:[NSURL fileURLWithPath:[path stringByAppendingString:@".metallib"]] error:&error];
        if(!lib) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
        id<MTLFunction> fn=[lib newFunctionWithName:@"main0"];if(!fn)return 1;
        id<MTLComputePipelineState> state=[device newComputePipelineStateWithFunction:fn error:&error];if(!state) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
        NSNumber *format=formats[row[@"vk_format"]];if(!format)return 1;
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format.unsignedIntegerValue width:8 height:4 mipmapped:NO];
        td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;td.storageMode=MTLStorageModeShared;
        id<MTLTexture> tex=[device newTextureWithDescriptor:td];if(!tex)return 1;
        [tex replaceRegion:MTLRegionMake2D(0,0,8,4) mipmapLevel:0 withBytes:initial.bytes bytesPerRow:initial.length/4];
        id<MTLArgumentEncoder> args=[fn newArgumentEncoderWithBufferIndex:0];if(!args)return 1;
        id<MTLBuffer> ab=[device newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];if(!ab)return 1;
        [args setArgumentBuffer:ab offset:0];NSDictionary *bindings=row[@"metal"];if(!bindings[@"img"])return 1;
        [args setTexture:tex atIndex:[bindings[@"img"] unsignedIntegerValue]];
        id<MTLBuffer> support=nil;
        if(bindings[@"support"]) { support=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];if(!support)return 1;
            for(unsigned i=0;i<1024;i++)((float*)support.contents)[i]=1;
            [args setBuffer:support offset:0 atIndex:[bindings[@"support"] unsignedIntegerValue]]; }
        id<MTLCommandBuffer> cmd=[queue commandBuffer];id<MTLComputeCommandEncoder> enc=[cmd computeCommandEncoder];if(!enc)return 1;
        [enc setComputePipelineState:state];[enc setBuffer:ab offset:0 atIndex:0];[enc useResource:tex usage:MTLResourceUsageRead|MTLResourceUsageWrite];
        if(support)[enc useResource:support usage:MTLResourceUsageRead];[enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[enc endEncoding];[cmd commit];[cmd waitUntilCompleted];
        if(cmd.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",cmd.error.description.UTF8String);return 1; }
        NSMutableData *out=[NSMutableData dataWithLength:initial.length];[tex getBytes:out.mutableBytes bytesPerRow:initial.length/4 fromRegion:MTLRegionMake2D(0,0,8,4) mipmapLevel:0];
        if(![out isEqualToData:expected]) { fprintf(stderr,"Storage surface mismatch: %s\n",[row[@"vk_format"] UTF8String]);return 1; }
        if(fwrite(out.bytes,1,out.length,stdout)!=out.length)return 1;
    } }
    fprintf(stderr,"Metal: %lu storage-surface formats matched\n",(unsigned long)rows.count);return 0;
} }
