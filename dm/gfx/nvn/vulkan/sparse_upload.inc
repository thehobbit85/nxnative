// The shared NVN recorder owns the virtual-to-physical page table. Its
// generation tracks remaps; the dirty tracker observes writes to the physical
// pages, even when several virtual textures refer to the same storage.
uint64_t nvn_sparse_gen(void);
int nvn_sparse_since(const void *pool, uint64_t address, uint64_t size, uint64_t generation, uint64_t stamp);
void nvn_sparse_protect(const void *pool, uint64_t address, uint64_t size);

static int sparse_changed(vtex *v, const tex_t *t) {
    if (!v->sparse || !v->uploaded) return 0;
    uint64_t mappings=nvn_sparse_gen(), writes=nvn_dirty_now();
    if (mappings==v->seen_mappings && writes==v->seen_writes) return 0;
    v->seen_mappings=mappings; v->seen_writes=writes;
    return nvn_sparse_since(t->pool,(uintptr_t)t->mem,t->L.size,v->uploaded_mappings,v->stamp);
}

// Gather only this mip of this array layer. A virtual address may name an
// inaccessible hole or lie beyond the host reservation. Read it through the
// page table and feed the existing mip-tail deswizzler a bounded snapshot.
static int sparse_deswizzle(const tex_t *t, unsigned level, unsigned layer, void *dst, size_t pitch) {
    uint64_t start=nvn_tex_level_offset(&t->d,level);
    uint64_t end=level+1<t->d.levels ? nvn_tex_level_offset(&t->d,level+1) : t->L.layer_size;
    if (end<=start || end>t->L.layer_size || layer>=t->L.layers) return -1;
    size_t bytes=(size_t)(end-start);
    uint64_t offset=layer*t->L.layer_size+start;
    if (offset>t->L.size || bytes>t->L.size-offset) return -1;
    uint8_t *snapshot=malloc(bytes);
    if (!snapshot) die("Vulkan: cannot allocate sparse mip snapshot of %zu bytes",bytes);
    nvn_sparse_copy(t->pool,snapshot,(uintptr_t)t->mem+offset,bytes);
    int result=nvn_deswizzle_from(&t->d,level,layer,snapshot,offset,dst,pitch);
    free(snapshot);
    return result;
}
