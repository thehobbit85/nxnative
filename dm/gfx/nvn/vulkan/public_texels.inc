// Public CPU reads run on arbitrary guest threads. Guest-owned textures can be
// read directly from their pool without constructing an image or using the
// submission thread's command buffers. Sparse holes read as zero.
int nvn_sparse_pool(const void *pool);
uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t addr, uint64_t size);

static void storage_bytes(const tex_t *t, int sparse, size_t offset, void *dst, size_t n) {
    if (sparse) nvn_sparse_copy(t->pool, dst, (uintptr_t)t->mem + offset, n);
    else memcpy(dst, t->mem + offset, n);
}

typedef struct {
    size_t x0, y0, bytes, rows;
    uint32_t nz, layer;
    int volume;
} texel_extent;

static int texel_region(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                        size_t row, size_t img, texel_extent *out) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || level >= 32 || level >= (t->d.levels ? t->d.levels : 1) ||
        r->width <= 0 || r->height <= 0 || r->depth < 0 || r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 ||
        r->xoffset % bw || r->yoffset % bh) return 0;
    int volume = t->d.target == NVN_TEXTURE_TARGET_3D;
    unsigned w = t->d.width >> level, h = t->d.height >> level, depth = t->d.depth >> level;
    if (t->d.target == NVN_TEXTURE_TARGET_1D || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) h = 1;
    uint32_t nz = r->depth ? (uint32_t)r->depth : 1;
    uint64_t layer = (uint64_t)layer0 + (uint32_t)r->zoffset;
    if ((uint64_t)r->xoffset + r->width > (w ? w : 1) || (uint64_t)r->yoffset + r->height > (h ? h : 1) ||
        layer + nz > (volume ? (depth ? depth : 1) : t->L.layers) || (volume && layer0)) return 0;
    size_t x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    size_t width = ((size_t)r->width + bw - 1) / bw * bpb, rows = ((size_t)r->height + bh - 1) / bh;
    if (width > row || row > SIZE_MAX / rows || img < (rows - 1) * row + width || img > SIZE_MAX / nz) return 0;
    *out = (texel_extent){x0, y0, width, rows, nz, (uint32_t)layer, volume};
    return 1;
}

static int storage_texels(const tex_t *t, unsigned level, uint8_t *dst, size_t row, size_t img, texel_extent ext) {
    if (!t->mem) return 0;
    size_t x0 = ext.x0, y0 = ext.y0, width = ext.bytes, rows = ext.rows;
    uint32_t nz = ext.nz, layer = ext.layer;
    int volume = ext.volume;
    int sparse = t->pool && nvn_sparse_pool(t->pool);
    if (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {
        size_t stride = t->L.layer_size / (t->d.height ? t->d.height : 1);
        if (level || layer || nz != 1 || x0 + width > stride) return 0;
        for (size_t y = 0; y < rows; ++y) storage_bytes(t, sparse, (y0 + y) * stride + x0, dst + y * row, width);
        return 1;
    }
    for (uint32_t z = 0; z < nz; ++z) {
        uint32_t bytes, block_rows, slices, block_bytes;
        unsigned th, td;
        uint64_t base;
        if (nvn_deswizzle_geom(&t->d, level, volume ? 0 : layer + z, &bytes, &block_rows, &slices,
                               &base, &th, &td, &block_bytes)) return 0;
        uint32_t pitch = nvn_tex_level_gobs(&t->d, level) * 64;
        if (pitch < bytes) pitch = bytes;
        // Up to 16 bytes remain contiguous before the next GOB sector.
        for (size_t y = 0; y < rows; ++y) for (size_t x = 0; x < width;) {
            uint32_t bx = x0 + x;
            size_t count = 16 - (bx & 15);
            if (count > width - x) count = width - x;
            uint64_t offset = base + nvn_bl_offset(bx, y0 + y, volume ? layer + z : 0, pitch, block_rows, th, td);
            if (offset > t->L.size || count > t->L.size - offset) return 0;
            storage_bytes(t, sparse, offset, dst + z * img + y * row + x, count);
            x += count;
        }
    }
    return 1;
}

// The public API may be called by a guest worker. One-time commands have
// their own command pool and use the same ordered VkQueue as submissions.
// Keep the image registry locked through completion so retirement cannot free it.
static int gpu_texels(const vtex *v, unsigned level, const NVNcopyRegion *r, const uint8_t *src, uint8_t *dst,
                      size_t row, size_t img, texel_extent ext) {
    if (v->f.dec || v->f.stencil || v->samples != VK_SAMPLE_COUNT_1_BIT) return 0;
    size_t slice = ext.bytes * ext.rows;
    VkDeviceMemory mem;
    void *mapped;
    VkBuffer buffer = buf_new(slice * ext.nz, src ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mem, &mapped);
    if (src) for (uint32_t z = 0; z < ext.nz; ++z) for (size_t y = 0; y < ext.rows; ++y)
        memcpy((uint8_t *)mapped + z * slice + y * ext.bytes, src + z * img + y * row, ext.bytes);
    VkBufferImageCopy copy = {0, 0, 0, {v->aspect, level, ext.volume ? 0 : ext.layer, ext.volume ? 1 : ext.nz},
                              {r->xoffset, r->yoffset, ext.volume ? (int32_t)ext.layer : 0},
                              {(uint32_t)r->width, (uint32_t)r->height, ext.volume ? ext.nz : 1}};
    VkCommandBuffer cb = once_begin();
    barrier_all(cb);
    if (src) {
        vkCmdCopyBufferToImage(cb, buffer, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        barrier_all(cb);
    } else vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
    once_end(cb);
    if (dst) for (uint32_t z = 0; z < ext.nz; ++z) for (size_t y = 0; y < ext.rows; ++y)
        memcpy(dst + z * img + y * row, (const uint8_t *)mapped + z * slice + y * ext.bytes, ext.bytes);
    vkUnmapMemory(g_dev, mem);
    vkDestroyBuffer(g_dev, buffer, NULL);
    vkFreeMemory(g_dev, mem, NULL);
    return 1;
}

// Explicit CPU writes still update guest memory. An image the GPU already
// owns needs the same region on-device; re-uploading its whole backing would
// discard render-target contents outside the region.
int dm_vk_write_owned_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                             const uint8_t *src, size_t row, size_t img) {
    pthread_mutex_lock(&g_rmu);
    vtex *v = tex_find(t->serial);
    int gpu = v && v->gpu;
    if (gpu) {
        texel_extent ext;
        if (!texel_region(t, level, layer0, r, row, img, &ext) || !gpu_texels(v, level, r, src, NULL, row, img, ext))
            RED_ONCE(81, "unsupported CPU write to GPU-owned texture (format %#x, samples %u)", t->d.format, t->d.samples);
    }
    pthread_mutex_unlock(&g_rmu);
    return gpu;
}
