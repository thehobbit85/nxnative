static int prepare_driver_tables(shader_t *shader) {
    unsigned used[DM_VK_MAX_BINDINGS]={0};uint32_t next_binding=0;
    int original_count=shader->nb;
    unsigned pool_slots=0,bounds_count=0,bounds_slots=0;
    for (int i=0;i<original_count;i++) {
        const sbind *binding=&shader->b[i];
        if (binding->binding>=next_binding) next_binding=binding->binding+1;
        if (binding->table || binding->kind==K_TABLE_MAP) used[binding->slot]=1;
        if (binding->pool && (unsigned)binding->slot+1>pool_slots) pool_slots=(unsigned)binding->slot+1;
        if (binding->kind==K_POOL_BOUNDS) { bounds_count++;bounds_slots=(unsigned)binding->reg; }
    }
    if (pool_slots && (bounds_count!=1 || bounds_slots<pool_slots)) return 0;
    for (int i=0;i<original_count;i++) {
        sbind *binding=&shader->b[i];
        if (!binding->logical_count) continue;
        unsigned slot=binding->pool ? (unsigned)binding->slot : 0;
        if (!binding->pool) while (slot<DM_VK_MAX_BINDINGS && used[slot]) slot++;
        if (slot==DM_VK_MAX_BINDINGS || shader->nb==DM_VK_MAX_BINDINGS) return 0;
        used[slot]=1;binding->slot=(int16_t)slot;binding->table=1;binding->count=1;
        shader->b[shader->nb++]=(sbind){.binding=next_binding++,.count=1,.type=BT_SSBO,.kind=K_TABLE_MAP,
            .slot=(int16_t)slot,.read_only=1};
        shader->has_tables=1;
    }
    return 1;
}
static uint32_t *driver_table_spv(uint32_t *words,size_t *bytes,const shader_t *shader) {
    descriptor_table_edit edits[DM_VK_MAX_BINDINGS];size_t count=0;
    for (int i=0;i<shader->nb;i++) {
        const sbind *binding=&shader->b[i];
        if (!binding->logical_count) continue;
        for (int j=0;j<shader->nb;j++) {
            const sbind *map=&shader->b[j];
            if (map->kind==K_TABLE_MAP && map->slot==binding->slot)
                edits[count++]=(descriptor_table_edit){binding->binding,map->binding,binding->pool ? 0 : binding->logical_count};
        }
    }
    if (!count) return words;
    size_t output_count;const char *reason;
    uint32_t *output=spv_descriptor_table(words,*bytes/4,edits,count,&output_count,&reason);
    free(words);
    if (!output) LOG("[RED] vk: shader %s driver table lowering failed: %s",shader->key,reason);
    else *bytes=output_count*4;
    return output;
}
