// Compare the exported probes with unmodified shared Apple translation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "resource_shapes_cases.h"
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=3)return 2;unsigned mode=(unsigned)strtoul(argv[2],NULL,10);if(mode>RS_MANY)return 2;
    NSString *directory=@(argv[1]),*key=@"00000000feed6100";NSError *error=nil;
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]]] error:&error];
    if(!library) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }id<MTLFunction> function=[library newFunctionWithName:@"main0"];if(!function)return 1;
    id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error];if(!pipeline) { fprintf(stderr,"%s\n",error.description.UTF8String);return 1; }
    id<MTLArgumentEncoder> encoder=[function newArgumentEncoderWithBufferIndex:0];if(!encoder)return 1;
    id<MTLBuffer> argumentBuffer=[device newBufferWithLength:encoder.encodedLength options:MTLResourceStorageModeShared];if(!argumentBuffer)return 1;[encoder setArgumentBuffer:argumentBuffer offset:0];
    id<MTLBuffer> buffers[30];for(unsigned i=0;i<30;i++) {
        buffers[i]=[device newBufferWithLength:i<10?65536:i==29?RS_WIDTH*RS_HEIGHT*4:32768 options:MTLResourceStorageModeShared];if(!buffers[i])return 1;
    }
    unsigned ni=mode==RS_MANY?40:1,width=mode==RS_MANY?1:RS_WIDTH,height=mode==RS_MANY?1:RS_HEIGHT;
    id<MTLTexture> images[40],texels[32];
    for(unsigned i=0;i<ni;i++) {
        MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:width height:height mipmapped:NO];desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
        images[i]=[device newTextureWithDescriptor:desc];if(!images[i])return 1;
    }
    for(unsigned i=0;i<32;i++) {
        for(unsigned x=0;x<256;x++)((uint32_t*)buffers[29].contents)[i*256+x]=rs_texel(RS_SLOTS,i,x,0);
        MTLTextureDescriptor *desc=[MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatR32Float width:256 resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead];
        texels[i]=[buffers[29] newTextureWithDescriptor:desc offset:i*1024 bytesPerRow:1024];if(!texels[i])return 1;
    }
    MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];sd.supportArgumentBuffers=YES;id<MTLSamplerState> sampler=[device newSamplerStateWithDescriptor:sd];if(!sampler)return 1;
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingString:@".reflection.json"]]];
    NSDictionary *reflection=[NSJSONSerialization JSONObjectWithData:data options:0 error:&error];if(!reflection)return 1;
    for(NSDictionary *binding in reflection[@"bindings"]) {
        NSString *kind=binding[@"kind"],*name=binding[@"msl_name"],*type=binding[@"msl_type"];NSUInteger index=[binding[@"arg_id"] unsignedIntegerValue];
        unsigned reg=[binding[@"reg"] respondsToSelector:@selector(unsignedIntValue)]?[binding[@"reg"] unsignedIntValue]:0;
        if([name hasSuffix:@"_cb0_40000000"]) {
            for(unsigned i=0;i<32;i++) {
                if([type hasPrefix:@"array<sampler,"]) [encoder setSamplerState:sampler atIndex:index+i];
                else if([type hasPrefix:@"array<texture_buffer<"]) [encoder setTexture:texels[i] atIndex:index+i];
                else return 1;
            }
        } else if([kind isEqualToString:@"cbuf"]&&reg<10) [encoder setBuffer:buffers[reg] offset:0 atIndex:index];
        else if([kind isEqualToString:@"sbuf"]&&reg<16) [encoder setBuffer:buffers[10+reg] offset:256 atIndex:index];
        else if([kind isEqualToString:@"sbuf_ubo"]&&([binding[@"off"] unsignedIntValue]==0||[binding[@"off"] unsignedIntValue]==56))
            [encoder setBuffer:buffers[[binding[@"off"] unsignedIntValue]?27:26] offset:256 atIndex:index];
        else if([kind isEqualToString:@"support"]) [encoder setBuffer:buffers[28] offset:0 atIndex:index];
        else if([kind isEqualToString:@"tex"]) { if(mode==RS_MANY&&reg>=40)return 1;[encoder setTexture:images[mode==RS_MANY?reg:0] atIndex:index]; }
        else if([kind hasPrefix:@"samp"]) [encoder setSamplerState:sampler atIndex:index];
        else return 1;
    }
    id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 1;
    for(unsigned test=0;test<rs_count(mode);test++) { @autoreleasepool {
        for(unsigned i=0;i<10;i++)rs_uniform(buffers[i].contents,mode,i,test);
        for(unsigned i=10;i<29;i++)memset(buffers[i].contents,0xcc,buffers[i].length);
        memset(buffers[28].contents,0,buffers[28].length);for(unsigned i=0;i<73*4;i++)((uint32_t*)buffers[28].contents)[48+i]=rs_bits(1);
        for(unsigned i=0;i<16;i++)((uint32_t*)buffers[10+i].contents)[64]=rs_storage(i,test);
        for(unsigned i=0;i<8;i++)((uint32_t*)buffers[26].contents)[64+i]=rs_slot(test,i);
        if(mode!=RS_SLOTS&&(test==0||mode==RS_MANY))for(unsigned i=0;i<ni;i++) {
            uint32_t *pixels=buffers[29].contents;for(unsigned x=0;x<width*height;x++)pixels[x]=rs_texel(mode,i,x,test);
            [images[i] replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:pixels bytesPerRow:width*4];
        }
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> compute=[command computeCommandEncoder];if(!command||!compute)return 1;
        [compute setComputePipelineState:pipeline];[compute setBuffer:argumentBuffer offset:0 atIndex:0];[compute setBuffer:buffers[28] offset:0 atIndex:1];
        for(unsigned i=0;i<30;i++)[compute useResource:buffers[i] usage:MTLResourceUsageRead|MTLResourceUsageWrite];
        for(unsigned i=0;i<ni;i++)[compute useResource:images[i] usage:MTLResourceUsageRead];for(unsigned i=0;i<32;i++)[compute useResource:texels[i] usage:MTLResourceUsageRead];
        [compute dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(mode==RS_MANY?1:8,1,1)];[compute endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"%s\n",command.error.description.UTF8String);return 1; }
        if(rs_check(mode,test,buffers[mode==RS_MANY?25:27].contents))return 1;
    } }
    fprintf(stderr,"Metal resource mode%u: %u dispatches passed\n",mode,rs_count(mode));return 0;
} }
