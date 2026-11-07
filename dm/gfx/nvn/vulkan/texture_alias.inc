// Match Metal's pool/offset/description identity for ordinary GPU images.
// The pool owns a shared image independently of the live texture-object IDs.
// Virtual physical-page aliases need separate sparse ownership handling.
#include "../nvn_hmap.h"
typedef struct { uint64_t pool;int64_t offset;nvn_texdesc description; } texture_alias_key;
static nvn_hmap g_texture_aliases={.klen=sizeof(texture_alias_key)};
static nvn_hmap g_texture_serials={.klen=sizeof(uint64_t)};
static nvn_hmap g_texture_keys={.klen=sizeof(uint64_t)};
static void tex_free(vtex *v);
static int texture_key(const tex_t *t,texture_alias_key *key) {
    if (!t->pool || !t->mem || t->has_layout || (t->pool->flags&NVN_MEMORY_POOL_FLAGS_VIRTUAL) ||
        t->d.target==NVN_TEXTURE_TARGET_BUFFER) return 0;
    memset(key,0,sizeof *key);key->pool=(uintptr_t)t->pool;key->offset=t->off;key->description=t->d;
    // IMAGE permits shader writes but does not change the storage layout.
    key->description.flags&=~NVN_TEXTURE_FLAGS_IMAGE;return 1;
}
static void texture_bind_serial(uint64_t serial,vtex *v) {
    vtex *old=nvn_hm_put(&g_texture_serials,&serial,v);
    if (old && old!=v) die("Vulkan: texture serial changed storage without finalization");
    if (!old) v->serial_refs++;
}
static vtex *tex_find(uint64_t serial) {
    vtex *v=nvn_hm_get(&g_texture_serials,&serial);
    if (v) return v;
    const texture_alias_key *key=nvn_hm_get(&g_texture_keys,&serial);
    v=key ? nvn_hm_get(&g_texture_aliases,key) : NULL;
    if (v) texture_bind_serial(serial,v);
    return v;
}
static vtex *texture_alias_find(const tex_t *t) {
    texture_alias_key key;
    vtex *v=texture_key(t,&key) ? nvn_hm_get(&g_texture_aliases,&key) : NULL;
    if (v) texture_bind_serial(t->serial,v);
    return v;
}
static void texture_alias_publish(const tex_t *t,vtex *v) {
    texture_alias_key key;
    if (v->alias_owned || !texture_key(t,&key)) return;
    vtex *old=nvn_hm_get(&g_texture_aliases,&key);
    // A previously uploaded CPU image may coexist with the GPU storage owner.
    // Do not replace that owner merely because the CPU object became writable.
    if (old && old!=v) return;
    nvn_hm_put(&g_texture_aliases,&key,v);v->alias_owned=1;
}
static void texture_retire_unused(vtex *v) {
    if (v->serial_refs || v->alias_owned) return;
    for (vtex **link=&g_tex[v->serial%NHASH];*link;link=&(*link)->next) if (*link==v) {
        *link=v->next;tex_free(v);return;
    }
}
static void texture_alias_drop_pool(const void *pool) {
    for (uint32_t i=0;i<g_texture_aliases.cap;) {
        if (!g_texture_aliases.vals[i]) { i++;continue; }
        texture_alias_key key;memcpy(&key,g_texture_aliases.keys+(size_t)i*sizeof key,sizeof key);
        if (key.pool!=(uintptr_t)pool) { i++;continue; }
        vtex *v=nvn_hm_del(&g_texture_aliases,&key);v->alias_owned=0;texture_retire_unused(v);
    }
    for (uint32_t i=0;i<g_texture_keys.cap;) {
        texture_alias_key *key=g_texture_keys.vals[i];
        if (!key || key->pool!=(uintptr_t)pool) { i++;continue; }
        uint64_t serial;memcpy(&serial,g_texture_keys.keys+(size_t)i*sizeof serial,sizeof serial);
        free(nvn_hm_del(&g_texture_keys,&serial));
    }
}
void mtl_tex_init(const tex_t *t) {
    texture_alias_key key;if (!texture_key(t,&key)) return;
    texture_alias_key *stored=malloc(sizeof *stored);if (!stored) die("Vulkan: texture identity allocation");
    memcpy(stored,&key,sizeof key);
    pthread_mutex_lock(&g_rmu);free(nvn_hm_put(&g_texture_keys,&t->serial,stored));pthread_mutex_unlock(&g_rmu);
}
void mtl_tex_release(tex_t *t) {
    pthread_mutex_lock(&g_rmu);
    free(nvn_hm_del(&g_texture_keys,&t->serial));
    vtex *v=nvn_hm_del(&g_texture_serials,&t->serial);
    if (v) { if (!v->serial_refs) die("Vulkan: texture serial ownership underflow");v->serial_refs--;texture_retire_unused(v); }
    pthread_mutex_unlock(&g_rmu);
}
