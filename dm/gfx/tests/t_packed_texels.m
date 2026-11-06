// Metal exposes these 16-bit formats as 2D textures. Use the NVN host's
// upload and sampled view as the reference for their channel order.
#include "../nvn/nvn_mtl.m"
#include "packed_texel_corpus.h"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

int main(void) { @autoreleasepool {
    mtl_init();
    size_t page=(size_t)getpagesize();
    uint8_t *mem=NULL; assert(!posix_memalign((void **)&mem,page,page));
    memset(mem,0,page);
    mtl_pool_add(mem,page);
    pool_t pool={.magic=MAGIC('P','O','O','L'),.mem=mem,.size=page,
                 .flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    NSError *error=nil;
    id<MTLLibrary> library=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void rd(texture2d<float,access::read> t [[texture(0)]],device float4* o [[buffer(0)]],uint i [[thread_position_in_grid]]) { o[i]=t.read(uint2(i,0)); }"
        options:nil error:&error];
    assert(library);
    id<MTLComputePipelineState> pipe=[g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"rd"] error:&error];
    assert(pipe);
    id<MTLBuffer> result=[g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    int failures=0, checked=0;
    for (size_t f=0;f<sizeof packed_texels/sizeof *packed_texels;f++) {
        tex_t t={.magic=MAGIC('T','E','X',0),.serial=nvn_new_serial(),.pool=&pool,.mem=mem,.has_layout=1,
                 .d={.target=NVN_TEXTURE_TARGET_2D,.format=packed_texels[f].format,.width=4,.height=1,.depth=1,.levels=1,.samples=1}};
        nvn_tex_layout(&t.d,&t.L); assert(t.L.layer_size<=page);
        assert(!nvn_swizzle(&t.d,0,0,packed_texel_words,sizeof packed_texel_words,mem));
        mtl_sub_begin(); mtl_tex_init(&t);
        for (int sw=0;sw<2;sw++) {
            view_t view={.set_swizzle=1,.swz={4,5,2,0}};
            mtl_reg_tex(501,&t,sw ? &view : NULL);
            os_unfair_lock_lock(&g_rmu);
            id<MTLTexture> tex=tex_by_id(501);
            os_unfair_lock_unlock(&g_rmu);
            assert(tex && tex.textureType==MTLTextureType2D);
            if (!sw) { mtl_sub_commit(); mtl_drain(); }
            id<MTLCommandBuffer> cb=q_cb(); id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
            [enc setComputePipelineState:pipe]; [enc setTexture:tex atIndex:0]; [enc setBuffer:result offset:0 atIndex:0];
            [enc dispatchThreads:MTLSizeMake(4,1,1) threadsPerThreadgroup:MTLSizeMake(4,1,1)];
            [enc endEncoding]; [cb commit]; [cb waitUntilCompleted]; assert(cb.status==MTLCommandBufferStatusCompleted);
            failures+=!packed_texel_matches(&packed_texels[f],sw,result.contents); checked++;
        }
        mtl_reg_tex(501,NULL,NULL); mtl_tex_release(&t);
    }
    mtl_pool_del(mem); mtl_drain(); free(mem);
    printf("Packed Metal texture views: %d cases, %d failures\n",checked,failures);
    return failures!=0;
} }
