// Resource ownership is observable without a GPU: retired images may leave the
// active tables only when no serial/pool owner remains. Actual fences are covered
// by the public NVN fixture shared with Metal.
#include "../../nvn_obj.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static void die(const char *format,...) { (void)format;abort(); }
typedef struct vtex { uint64_t serial,len,alias_seen;unsigned serial_refs;int alias_owned;struct vtex *next; } vtex;
#define NHASH 64
static vtex *g_tex[NHASH];static pthread_mutex_t g_rmu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence;static unsigned retired;
static void tex_free(vtex *v) { (void)v;retired++; }
#include "../texture_alias.inc"
static tex_t description(pool_t *pool,uint64_t serial,int64_t offset,unsigned width) {
    tex_t t={.pool=pool,.mem=pool->mem+offset,.off=offset,.serial=serial,
        .d={.width=width,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&t.d,&t.L);return t;
}
static void publish(vtex *image,const tex_t *t) {
    *image=(vtex){.serial=t->serial,.len=t->L.size};
    image->next=g_tex[image->serial%NHASH];g_tex[image->serial%NHASH]=image;
    mtl_tex_init(t);texture_bind_serial(t->serial,image);texture_alias_publish(t,image);
}
int main(void) {
    uint8_t memory[0x4000]={0};pool_t pool={.mem=memory,.size=sizeof memory},other=pool;
    tex_t a=description(&pool,1,0,8),b=description(&pool,2,0,16),far=description(&pool,3,0x2000,8),c=description(&other,4,0,8);
    vtex av,bv,fv,cv;publish(&av,&a);publish(&fv,&far);publish(&cv,&c);
    mtl_tex_release(&a);mtl_tex_release(&far);mtl_tex_release(&c);assert(retired==0);
    g_fence=15;publish(&bv,&b);assert(retired==0); // recently accessed overlap retained
    mtl_tex_release(&b);g_fence=32;
    tex_t fresh=description(&pool,5,0,24);vtex nv;publish(&nv,&fresh);
    assert(retired==2);assert(!texture_alias_find(&a) && !texture_alias_find(&b));
    assert(texture_alias_find(&far)==&fv && texture_alias_find(&c)==&cv);
    // A live serial protects a differently shaped overlapping image indefinitely.
    g_fence=64;tex_t wider=description(&pool,6,0,32);vtex wv;publish(&wv,&wider);
    assert(retired==2 && texture_alias_find(&fresh)==&nv);
    // Looking up an existing serial refreshes the image's use generation.
    g_fence=80;assert(tex_find(fresh.serial)==&nv);mtl_tex_release(&fresh);
    tex_t recent=description(&pool,7,0,40);vtex rv;publish(&rv,&recent);assert(retired==2);
    // Pool eviction cannot retire images still held by a live serial.
    texture_alias_drop_pool(&pool);assert(retired==3);
    mtl_tex_release(&far);mtl_tex_release(&wider);mtl_tex_release(&recent);assert(retired==6);
    texture_alias_drop_pool(&other);assert(retired==6);mtl_tex_release(&c);assert(retired==7);
    assert(!g_texture_aliases.n && !g_texture_serials.n && !g_texture_keys.n);
    for (unsigned i=0;i<NHASH;i++) assert(!g_tex[i]);
    free(g_texture_aliases.keys);free(g_texture_aliases.vals);free(g_texture_serials.keys);free(g_texture_serials.vals);
    free(g_texture_keys.keys);free(g_texture_keys.vals);
    puts("Texture ownership retained recent/live/other-pool images and retired stale overlaps and finalized pools");
    return 0;
}
