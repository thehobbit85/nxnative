// The pool's guest ID space can be sparse and much larger than the driver's
// descriptor budget. Deduplicate equivalent registered resources and retain
// only a compact live descriptor array plus a logical-index map.
static uint64_t pool_registration_hash(int sampler,unsigned id) {
    if (sampler) return fnv(&g_sreg[id],sizeof g_sreg[id]);
    const treg_t *texture=&g_treg[id];
    uint64_t hash=fnv(&texture->t,sizeof texture->t) ^ texture->has_v;
    if (texture->has_v) hash^=fnv(&texture->v,sizeof texture->v);
    return hash;
}
static int same_pool_registration(int sampler,unsigned a,unsigned b) {
    if (sampler) return !memcmp(&g_sreg[a],&g_sreg[b],sizeof g_sreg[a]);
    const treg_t *first=&g_treg[a],*second=&g_treg[b];
    return !memcmp(&first->t,&second->t,sizeof first->t) && first->has_v==second->has_v &&
        (!first->has_v || !memcmp(&first->v,&second->v,sizeof first->v));
}
static int build_resource_pool(handle_table *table,const sbind *binding) {
    int sampler=binding->type==BT_SAMP;
    int base=sampler ? S.samp_base : S.tex_base;
    pthread_mutex_lock(&g_rmu);
    if (table->pool_kind==sampler+1 && table->pool_base==base && table->registration_generation==g_registration_generation) {
        pthread_mutex_unlock(&g_rmu);return 1;
    }
    int available=(sampler ? g_nsreg : g_ntreg)-base;
    uint32_t length=available>0 ? (uint32_t)available : 1;
    if (length>binding->logical_count) length=binding->logical_count;
    uint32_t live=0;
    for (uint32_t i=0;i<length;i++) {
        uint64_t id=(uint64_t)(unsigned)base+i;
        if (id && id<(unsigned)(sampler ? g_nsreg : g_ntreg) &&
            (sampler ? g_sreg_ok[id] : id<=0xfffff && g_treg[id].ok)) live++;
    }
    if (!table_reserve(table,length,live+1)) { pthread_mutex_unlock(&g_rmu);return 0; }
    uint32_t buckets=16;
    while (buckets<2*live) buckets*=2;
    uint32_t *slots=calloc(buckets,sizeof *slots);
    if (!slots) { pthread_mutex_unlock(&g_rmu);return 0; }
    table->count=1;table->handles[0]=0;table->map[0]=length;table->map[length+1]=0;
    for (uint32_t i=0;i<length;i++) {
        uint64_t id=(uint64_t)(unsigned)base+i;
        uint32_t slot=0;
        if (id && id<(unsigned)(sampler ? g_nsreg : g_ntreg) &&
            (sampler ? g_sreg_ok[id] : id<=0xfffff && g_treg[id].ok)) {
            uint32_t bucket=(uint32_t)pool_registration_hash(sampler,(unsigned)id)&(buckets-1);
            while (slots[bucket]) {
                uint64_t registered=table->handles[slots[bucket]];
                unsigned other=(unsigned)(sampler ? registered>>20 : registered);
                if (same_pool_registration(sampler,(unsigned)id,other)) break;
                bucket=(bucket+1)&(buckets-1);
            }
            if (!slots[bucket]) {
                slots[bucket]=table->count++;
                table->handles[slots[bucket]]=sampler ? id<<20 : id;
            }
            slot=slots[bucket];
        }
        table->map[1+i]=slot;
    }
    free(slots);
    table->pool_kind=sampler+1;table->pool_base=base;table->registration_generation=g_registration_generation;
    pthread_mutex_unlock(&g_rmu);return 1;
}
