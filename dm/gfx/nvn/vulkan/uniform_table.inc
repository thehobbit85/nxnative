// A shader can address 8192 uniform-buffer handles. The per-draw descriptor
// layout only contains distinct live pairs. Its companion SSBO preserves the
// guest indices and maps empty/out-of-range elements to descriptor zero.
enum { TABLE_ELEMENTS = 8192 };
typedef struct {
    uint64_t *handles;
    uint32_t *map, count, handle_capacity, map_capacity;
    uint64_t registration_generation;
    int pool_base, pool_kind;
} handle_table;
static handle_table *table_cache[6][DM_VK_MAX_BINDINGS]; // replay thread scratch
static uint64_t pool_remaining(uint64_t addr);
static int table_reserve(handle_table *table,uint32_t length,uint32_t handles) {
    if (table->map_capacity<length+2) {
        uint32_t *map=realloc(table->map,(size_t)(length+2)*sizeof *map);
        if (!map) return 0;
        table->map=map;table->map_capacity=length+2;
    }
    if (table->handle_capacity<handles) {
        uint64_t *values=realloc(table->handles,(size_t)handles*sizeof *values);
        if (!values) return 0;
        table->handles=values;table->handle_capacity=handles;
    }
    return 1;
}
#include "resource_pools.inc"

typedef struct { const uint8_t *bytes; uint32_t length; } table_source;
static uint32_t table_word(table_source source, uint32_t i) {
    uint32_t value = 0;
    if (i < source.length) memcpy(&value, source.bytes + 8*i, sizeof value);
    return value;
}
static table_source table_words(int stage, int reg, int off) {
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || (uint32_t)off >= u->size || u->addr > UINT64_MAX-(uint32_t)off) return (table_source){0};
    uint64_t addr = u->addr+(uint32_t)off, bytes = u->size-(uint32_t)off;
    uint64_t mapped = pool_remaining(addr);
    if (bytes > mapped) bytes = mapped;
    table_source source = {(const uint8_t *)(uintptr_t)addr, bytes/8 < TABLE_ELEMENTS ? (uint32_t)(bytes/8) : TABLE_ELEMENTS};
    while (source.length && !table_word(source, source.length-1)) source.length--;
    return source;
}
static int build_table(int stage, const sbind *b) {
    handle_table **entry = &table_cache[stage][b->slot];
    if (!*entry) *entry = calloc(1,sizeof **entry);
    handle_table *table = *entry;
    if (!table) return 0;
    if (b->pool) return build_resource_pool(table,b);
    table->pool_kind=0;
    table_source texture = {0}, sampler = {0};
    if (!b->logical_count) {
        if (b->type != BT_SAMP) texture = table_words(stage,b->reg,b->off);
        if (b->type != BT_TEX) sampler = table_words(stage,b->sreg,b->soff);
    }
    uint32_t n = texture.length > sampler.length ? texture.length : sampler.length;
    if (b->logical_count) n=b->logical_count;
    if (!table_reserve(table,n,n+1)) return 0;
    uint16_t slots[2*TABLE_ELEMENTS] = {0};
    table->handles[0] = 0; table->count = 1; table->map[0] = n; table->map[n+1] = 0;
    uint64_t fixed = b->driver_sampler < 0 ? 0 :
        dm_vk_c2_handle(S.tex[stage],S.img[stage],S.samp[stage],(uint32_t)b->driver_sampler) >> 20;
    pthread_mutex_lock(&g_rmu);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tid = table_word(texture,i) & 0xfffff, sid = table_word(sampler,i) >> 20;
        uint64_t texture_id = tid ? (uint64_t)tid + S.tex_base : 0;
        uint64_t sampler_id = b->driver_sampler >= 0 ? (i < sampler.length ? fixed : 0) : (sid ? (uint64_t)sid + S.samp_base : 0);
        if (b->logical_count) {
            uint64_t handle=sampled_handle(stage,b,i);
            texture_id=b->type==BT_SAMP ? 0 : handle&0xfffff;
            sampler_id=b->type==BT_TEX || b->type==BT_UTB || b->type==BT_TBUF ? 0 : handle>>20;
        }
        if (texture_id >= (uint32_t)g_ntreg || texture_id > 0xfffff || !g_treg[texture_id].ok) texture_id = 0;
        if (sampler_id >= (uint32_t)g_nsreg || !g_sreg_ok[sampler_id]) sampler_id = 0;
        uint64_t handle = texture_id | (sampler_id << 20);
        uint32_t slot = 0;
        if (handle) {
            uint32_t bucket = (uint32_t)((handle * UINT64_C(11400714819323198485)) >> 50);
            while (slots[bucket] && table->handles[slots[bucket]] != handle)
                bucket = (bucket+1) % (2*TABLE_ELEMENTS);
            if (!slots[bucket]) {
                slots[bucket] = (uint16_t)table->count++;
                table->handles[slots[bucket]] = handle;
            }
            slot = slots[bucket];
        }
        table->map[1+i] = slot;
    }
    pthread_mutex_unlock(&g_rmu);
    return 1;
}
static uint64_t descriptor_handle(int stage, const sbind *b, uint32_t element) {
    return b->table ? table_cache[stage][b->slot]->handles[element] : sampled_handle(stage,b,element);
}
static int prepare_tables(shader_t **selected) {
    shader_t *base = *selected;
    if (!base || !base->has_tables) return 1;
    for (int i=0; i<base->nb; i++)
        if (base->b[i].table && !build_table(base->guest_stage,&base->b[i])) return 0;
    shader_t *s = base;
    while (s) {
        int match = 1;
        for (int i=0; i<s->nb; i++) {
            const sbind *b = &s->b[i];
            if (b->table && b->count != table_cache[s->guest_stage][b->slot]->count) { match = 0; break; }
        }
        if (match) { *selected=s; return s->dsl != VK_NULL_HANDLE; }
        s = s == base ? base->table_variants : s->next_table_variant;
    }
    s = malloc(sizeof *s);
    if (!s) return 0;
    *s = *base;
    s->buffer_variants=NULL; s->vertex_variants=NULL; s->wrapped_variants=NULL;
    s->fragment_variants=NULL; s->cpp=VK_NULL_HANDLE; s->cpl=VK_NULL_HANDLE;
    for (int i=0; i<s->nb; i++) {
        sbind *b=&s->b[i];
        if (b->table) b->count = table_cache[s->guest_stage][b->slot]->count;
    }
    s->dsl=shader_layout(s);
    s->next_table_variant=base->table_variants; base->table_variants=s;
    if (!s->dsl) { LOG("[RED] vk: uniform handle table layout for %s exceeds device limits",s->key); return 0; }
    *selected=s;
    return 1;
}
