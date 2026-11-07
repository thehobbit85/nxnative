// Resource ownership is observable without a GPU: retired images may leave the
// active tables only when no serial/pool owner remains. Actual fences are covered
// by the public NVN fixture shared with Metal.
#define _GNU_SOURCE
#include "../../nvn_obj.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
static void die(const char *format,...) { (void)format;abort(); }
typedef struct vtex { uint64_t serial,len,alias_seen;unsigned serial_refs;int alias_owned;void *package_key;struct vtex *next; } vtex;
#define NHASH 64
static vtex *g_tex[NHASH];static pthread_mutex_t g_rmu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence;static unsigned retired;
static void tex_free(vtex *v) { (void)v;retired++; }
#include "../texture_alias.inc"
static tex_t description(pool_t *pool,uint64_t serial,int64_t offset,unsigned width) {
    tex_t t={.magic=MAGIC('T','E','X',0),.pool=pool,.mem=pool->mem+offset,.off=offset,.serial=serial,
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
    // Engine-owned copies have the same serial but cannot finalize its owner.
    tex_t copy=a;mtl_tex_release(&copy);
    assert(av.serial_refs==1 && tex_find(a.serial)==&av);
    // This also applies to packaged descriptions excluded from pool aliases.
    tex_t packaged=description(&pool,8,0,8);packaged.has_layout=1;
    vtex pv;publish(&pv,&packaged);copy=packaged;mtl_tex_release(&copy);
    assert(pv.serial_refs==1 && tex_find(packaged.serial)==&pv);
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
    mtl_tex_release(&packaged);assert(retired==8);
    tex_t moved=description(&pool,9,0,8);moved.has_layout=1;vtex mv;publish(&mv,&moved);
    copy=moved;moved.magic=0;mtl_tex_release(&copy);assert(retired==9);
    tex_t *unmapped=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(unmapped!=MAP_FAILED);
    *unmapped=description(&pool,10,0,8);unmapped->has_layout=1;vtex uv;publish(&uv,unmapped);
    copy=*unmapped;assert(!munmap(unmapped,16384));mtl_tex_release(&copy);assert(retired==10);
    tex_t upload=description(&pool,11,0,8);upload.has_layout=1;vtex up;publish(&up,&upload);texture_package_publish(&upload,&up);
    tex_t alias=upload;alias.serial=12;alias.d.flags|=NVN_TEXTURE_FLAGS_IMAGE;mtl_tex_init(&alias);
    assert(texture_package_find(&alias)==&up && up.serial_refs==2);
    tex_t different=alias;different.layout[1]=1;assert(!texture_package_find(&different));
    different=alias;different.d.format=NVN_FORMAT_RGBA8_SRGB;assert(!texture_package_find(&different));
    mtl_tex_release(&upload);assert(retired==10 && g_texture_packages.n==1);
    mtl_tex_release(&alias);assert(retired==11 && !g_texture_packages.n);
    // Pool removal evicts a weak key even while a serial keeps its image alive.
    upload.serial=13;publish(&up,&upload);texture_package_publish(&upload,&up);
    texture_alias_drop_pool(&pool);assert(!g_texture_packages.n && up.serial_refs==1 && !texture_package_find(&upload));
    alias.serial=14;vtex replacement;publish(&replacement,&alias);texture_package_publish(&alias,&replacement);
    mtl_tex_release(&upload);assert(retired==12 && texture_package_find(&alias)==&replacement);
    mtl_tex_release(&alias);assert(retired==13 && !g_texture_packages.n);
    assert(!g_texture_aliases.n && !g_texture_serials.n && !g_texture_keys.n && !g_texture_live.n);
    for (unsigned i=0;i<NHASH;i++) assert(!g_tex[i]);
    free(g_texture_aliases.keys);free(g_texture_aliases.vals);free(g_texture_serials.keys);free(g_texture_serials.vals);
    free(g_texture_keys.keys);free(g_texture_keys.vals);
    free(g_texture_live.keys);free(g_texture_live.vals);
    free(g_texture_packages.keys);free(g_texture_packages.vals);
    puts("Texture ownership retained live owners, shared exact packaged storage weakly, and retired stale overlaps, finalized pools and moved/unmapped owners");
    return 0;
}
