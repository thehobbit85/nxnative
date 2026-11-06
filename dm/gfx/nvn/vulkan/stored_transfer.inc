// Preserve the original bytes of textures the guest can read or re-upload.
// BC endpoint and selector bits cannot be reconstructed from decoded pixels.
static int mirror_upload(const tex_t *t, const vtex *v) {
    if (v->f.dec) return 1;
    if (!t->mem || !t->pool || v->gpu ||
        (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) ||
        (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE))) return 0;
    return v->uploaded || nvn_ft_tracked(t->serial);
}
// The submission ring contains the source blocks in guest order. Save them
// to the pool, then update just this region of its native sampled image.
static int write_stored_image(const tex_t *t, vtex *v, VkBufferImageCopy region, texel_extent ext, VkDeviceSize off) {
    uint8_t *raw=C->ringp+off;
    size_t slice=ext.bytes*ext.rows;
    if (!storage_texels(t,region.imageSubresource.mipLevel,raw,ext.bytes,slice,ext,1)) return 0;
    g_wgen++;   // a subsequent buffer binding can alias the mirrored storage
    if (v->f.dec) {
        size_t decoded_slice=(size_t)region.imageExtent.width*region.imageExtent.height*dec_bpp(v->f.dec);
        region.bufferOffset=ring_alloc(decoded_slice*ext.nz,16);
        for (uint32_t z=0; z<ext.nz; ++z)
            bc_decode(v->f.dec,raw+z*slice,ext.bytes,region.imageExtent.width,region.imageExtent.height,C->ringp+region.bufferOffset+z*decoded_slice);
        vkCmdCopyBufferToImage(C->cb,C->ring,v->img,VK_IMAGE_LAYOUT_GENERAL,1,&region);
        barrier_all(C->cb);
    } else {
        VkDescriptorBufferInfo guest={C->ring,off,slice*ext.nz};
        buffer_image_transfer(v,region,guest,1,transfer_alignment(v));
    }
    if (v->span) {
        // This texture already received these bytes. Other views of the same
        // guest pages still see the write through their own dirty stamps.
        v->stamp=nvn_dirty_now(); nvn_dirty_protect(t->mem,t->L.size);
    }
    return 1;
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
    return write_stored_image(t,v,l->region,ext,off);
}
// Read encoded blocks before modifying the destination, including when the
// two texture objects overlap in the guest pool. Ignore queue buffer pitches
// for an image-to-image copy.
static int copy_stored_blocks(const p_copy_t2t *c, VkImageCopy region) {
    int bw, bh, dbw, dbh;
    int bpb=nvn_fmt_bpb(c->src->d.format,&bw,&bh), dbpb=nvn_fmt_bpb(c->dst->d.format,&dbw,&dbh);
    if (!bpb || bpb!=dbpb || bw!=dbw || bh!=dbh || !c->src->mem || !c->dst->mem) return 0;
    size_t row=((size_t)c->sr.width+bw-1)/bw*bpb, slice=row*(((size_t)c->sr.height+bh-1)/bh);
    texel_extent src, dst;
    unsigned src_base=c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0;
    unsigned dst_base=c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    if (!texel_region(c->src,region.srcSubresource.mipLevel,src_base,&c->sr,row,slice,&src) ||
        !texel_region(c->dst,region.dstSubresource.mipLevel,dst_base,&c->dr,row,slice,&dst)) return 0;
    cpu_ready((uintptr_t)c->src->mem,c->src->L.size);
    cpu_access_ready((uintptr_t)c->dst->mem,c->dst->L.size,1);
    vtex *s=tex_get(c->src,0), *d=tex_get(c->dst,0);
    if (!s || !d || s->gpu || d->gpu) return 0;
    VkDeviceSize off=ring_alloc(slice*src.nz,transfer_alignment(d));
    if (!storage_texels(c->src,region.srcSubresource.mipLevel,C->ringp+off,row,slice,src,0)) return 0;
    VkBufferImageCopy out={0,0,0,region.dstSubresource,region.dstOffset,region.extent};
    return write_stored_image(c->dst,d,out,dst,off);
}
