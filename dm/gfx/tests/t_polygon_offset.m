#include "../nvn/nvn_mtl.m"
#include "recorded_list.h"
#include "polygon_offset_corpus.h"
#include "polygon_fill_corpus.h"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { float2 p=float2((i<<1)&2,i&2);return float4(2*p-1,.25+.25*p.x,1); }\n"
        "vertex float4 wire(uint i[[vertex_id]]) { const float2 p[]={float2(-.6875,-.6875),float2(.6875,-.6875),float2(0,.6875)}; return float4(p[i],.25,1); }\n"
        "fragment float f(float4 p[[position]]) { return p.z; }" options:nil error:&error];
    assert(lib);
    prog_t pg={.serial=0x8142,.nstage=2};
    NvProg *pr=[NvProg new]; NvShader *vs=[NvShader new], *fs=[NvShader new];
    vs->fn=fn4_note([lib newFunctionWithName:@"v"],lib,@"v",nil);
    fs->fn=fn4_note([lib newFunctionWithName:@"f"],lib,@"f",nil);
    fs->stage=1; pr->sh[0]=vs; pr->sh[1]=fs; serial_put(pg.serial,pr);
    polygon_offset(&pg);
    prog_t wire={.serial=0x81e1,.nstage=2};
    NvProg *wp=[NvProg new]; NvShader *wv=[NvShader new];
    wv->fn=fn4_note([lib newFunctionWithName:@"wire"],lib,@"wire",nil);
    wp->sh[0]=wv; wp->sh[1]=fs; serial_put(wire.serial,wp);
    polygon_fill(&wire,1);
    return 0;
} }
