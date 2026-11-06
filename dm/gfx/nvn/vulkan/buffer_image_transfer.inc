// Guest pitches are byte counts. A slice can start partway through what
// Vulkan would consider a row, and the padding can be unmapped guest memory.
typedef struct {
    VkBufferImageCopy region;
    uint32_t slices;
    size_t bytes, rows, row_pitch, image_pitch, span;
} buffer_image_layout;

static int buffer_image_region(const vtex *v, const p_copy_b2t *c, buffer_image_layout *layout) {
    const NVNcopyRegion *r = &c->r;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    VkBufferImageCopy region = {0};
    if (!bpb || !c->src || r->xoffset % bw || r->yoffset % bh ||
        S.copy_stride < 0 || S.copy_img_stride < 0 || v->samples != VK_SAMPLE_COUNT_1_BIT ||
        !texture_copy_region(v, c->has_view ? &c->view : NULL, r, &region.imageSubresource,
                             &region.imageOffset, &region.imageExtent)) return 0;
    uint32_t slices = r->depth ? (uint32_t)r->depth : 1;
    size_t bytes = ((size_t)r->width + bw - 1) / bw * bpb, rows = ((size_t)r->height + bh - 1) / bh;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : bytes;
    if (row < bytes || row > SIZE_MAX / rows) return 0;
    size_t image = S.copy_img_stride ? (size_t)S.copy_img_stride : row * rows;
    size_t end = (rows - 1) * row + bytes;
    if (image < end || (slices > 1 && image > (SIZE_MAX - end) / (slices - 1))) return 0;
    size_t span = (slices - 1) * image + end;
    if (c->src > UINTPTR_MAX - (span-1)) return 0;
    *layout = (buffer_image_layout){region, slices, bytes, rows, row, image, span};
    return 1;
}

static VkDeviceSize upload_rows(uint64_t addr, const buffer_image_layout *l, VkDeviceSize align) {
    size_t slice = l->bytes * l->rows, size = slice * l->slices;
    if (l->span == size) return copy_in(addr, size, align);
    gw_wait_prior(addr, l->span);
    VkDeviceSize off = ring_alloc(size, align);
    for (uint32_t z=0; z<l->slices; ++z) for (size_t y=0; y<l->rows; ++y)
        memcpy(C->ringp + off + z*slice + y*l->bytes,
               (const uint8_t *)(uintptr_t)addr + z*l->image_pitch + y*l->row_pitch, l->bytes);

    // GPU-produced rows are still in their submission buffers. Overlay them
    // in write order without waiting for the CPU or touching the guest gaps.
    pthread_mutex_lock(&g_gwmu);
    int started = 0;
    for (size_t i=0; i<g_ngw; ++i) {
        const gw_t *w = &g_gw[i];
        if (!overlaps(addr, l->span, w->addr, w->size)) continue;
        if (!started) { barrier_all(C->cb); started = 1; }
        int copied = 0;
        for (uint32_t z=0; z<l->slices; ++z) for (size_t y=0; y<l->rows; ++y)
            copied |= snapshot_overlay(w, addr + z*l->image_pitch + y*l->row_pitch, l->bytes,
                                       off + z*slice + y*l->bytes);
        if (copied) barrier_all(C->cb);
    }
    pthread_mutex_unlock(&g_gwmu);
    return off;
}

static void publish_rows(uint64_t destination, const buffer_image_layout *l, VkBuffer buffer, VkDeviceSize off, uint8_t *mapped) {
    size_t slice = l->bytes*l->rows, size = slice*l->slices;
    if (l->span == size) buffer_written(destination, size, buffer, off, mapped + off);
    else for (uint32_t z=0; z<l->slices; ++z) {
        uint64_t addr = destination + z*l->image_pitch;
        VkDeviceSize start = off + z*slice;
        if (l->bytes == l->row_pitch) buffer_written(addr, slice, buffer, start, mapped + start);
        else for (size_t y=0; y<l->rows; ++y) {
            VkDeviceSize pos = start + y*l->bytes;
            buffer_written(addr + y*l->row_pitch, l->bytes, buffer, pos, mapped + pos);
        }
    }
}

#include "packed_depth.inc"

static VkDeviceSize transfer_alignment(const vtex *v) {
    VkDeviceSize align = g_props.limits.minStorageBufferOffsetAlignment;
    return v->f.stencil && align > 16 ? align : 16;
}

#include "stored_transfer.inc"

static void copy_b2t(const p_copy_b2t *c) {
    if (!c->tex || (vk_skip() & SK_B2T)) return;
    if (g_plog) LOG("plog:  b2t serial %llu fmt %#x %dx%d at %d,%d,%d", (unsigned long long)c->tex->serial, c->tex->d.format, c->r.width, c->r.height, c->r.xoffset, c->r.yoffset, c->r.zoffset);
    vtex *v = tex_get(c->tex, 0);
    if (!v) return;
    buffer_image_layout l;
    if (!buffer_image_region(v, c, &l)) { RED_ONCE(50, "invalid buffer-to-texture region or strides"); return; }
    pass_end();
    if (mirror_upload(c->tex,v)) {
        if (!stored_transfer(c,&l,1)) RED_ONCE(50,"unsupported upload storage for format %#x",c->tex->d.format);
        return;
    }
    VkDeviceSize align = transfer_alignment(v);
    VkDeviceSize off = upload_rows(c->src, &l, align);
    VkDescriptorBufferInfo guest = {C->ring,off,l.bytes*l.rows*l.slices};
    buffer_image_transfer(v,l.region,guest,1,align);
    v->gpu = 1;
}

static void copy_t2b(const p_copy_b2t *c) {
    if (!c->tex || (vk_skip() & SK_T2B)) return;
    pass_end();
    vtex *v = tex_get(c->tex, 0);
    if (!v) return;
    buffer_image_layout l;
    if (!buffer_image_region(v, c, &l)) { RED_ONCE(51, "invalid texture-to-buffer region or strides"); return; }
    if (v->f.dec) {
        if (!stored_transfer(c,&l,0)) RED_ONCE(51,"unavailable original BC blocks for format %#x",c->tex->d.format);
        return;
    }
    size_t slice = l.bytes * l.rows, size = slice * l.slices;
    VkDeviceSize align = transfer_alignment(v), off = ring_alloc(size, align);
    VkDescriptorBufferInfo guest = {C->ring,off,size};
    uint8_t *mapped = C->ringp;
    buffer_image_transfer(v,l.region,guest,0,align);
    publish_rows(c->src,&l,guest.buffer,off,mapped);
}
