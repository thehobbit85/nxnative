// Preserve the original bytes of textures the guest can read or re-upload.
// BC endpoint and selector bits cannot be reconstructed from decoded pixels.
static int mirror_upload(const tex_t *t, const vtex *v) {
    if (v->f.dec) return 1;
    if (!t->mem || !t->pool || v->gpu ||
        (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) ||
        (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE))) return 0;
    return v->uploaded || nvn_ft_tracked(t->serial);
}
static int stored_transfer(const p_copy_b2t *c, const buffer_image_layout *l, int upload) {
    const tex_t *t = c->tex;
    unsigned level = l->region.imageSubresource.mipLevel;
    unsigned base = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t slice = l->bytes * l->rows, total = slice * l->slices;
    texel_extent ext;
    if (!t->mem || !texel_region(t,level,base,&c->r,l->bytes,slice,&ext)) return 0;
    // An earlier GPU write must land before the CPU decoder reads the
    // source, or before the original blocks are read from aliased storage.
    if (upload) cpu_ready(c->src,l->span);
    cpu_access_ready((uintptr_t)t->mem,t->L.size,upload);
    vtex *v = tex_get(t,0);  // the dependency can have started a new segment
    if (!v || (!upload && v->gpu)) return 0;
    if (!upload) {
        VkDeviceSize off = ring_alloc(total,16);
        if (!storage_texels(t,level,C->ringp+off,l->bytes,slice,ext,0)) return 0;
        publish_rows(c->src,l,C->ring,off,C->ringp);
        return 1;
    }
    VkDeviceSize align=transfer_alignment(v), off=ring_alloc(total,align);
    uint8_t *raw=C->ringp+off;
    for (uint32_t z=0; z<l->slices; ++z) for (size_t y=0; y<l->rows; ++y)
        memcpy(raw+z*slice+y*l->bytes, (const uint8_t *)(uintptr_t)c->src+z*l->image_pitch+y*l->row_pitch, l->bytes);
    if (!storage_texels(t,level,raw,l->bytes,slice,ext,1)) return 0;
    g_wgen++;   // a subsequent buffer binding can alias the mirrored storage
    VkBufferImageCopy region = l->region;
    if (v->f.dec) {
        size_t decoded_slice=(size_t)c->r.width*c->r.height*dec_bpp(v->f.dec);
        region.bufferOffset=ring_alloc(decoded_slice*l->slices,16);
        for (uint32_t z=0; z<l->slices; ++z)
            bc_decode(v->f.dec,raw+z*slice,l->bytes,c->r.width,c->r.height,C->ringp+region.bufferOffset+z*decoded_slice);
        vkCmdCopyBufferToImage(C->cb,C->ring,v->img,VK_IMAGE_LAYOUT_GENERAL,1,&region);
        barrier_all(C->cb);
    } else {
        VkDescriptorBufferInfo guest={C->ring,off,total};
        buffer_image_transfer(v,region,guest,1,align);
    }
    if (v->span) {
        // This texture already received these bytes. Other views of the same
        // guest pages still see the write through their own dirty stamps.
        v->stamp=nvn_dirty_now(); nvn_dirty_protect(t->mem,t->L.size);
    }
    return 1;
}
