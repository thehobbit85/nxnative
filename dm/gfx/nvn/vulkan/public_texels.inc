// Public CPU reads run on arbitrary guest threads. Guest-owned textures can be
// read directly from their pool without constructing an image or using the
// submission thread's command buffers. Sparse holes read as zero.
int nvn_sparse_pool(const void *pool);
uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t addr, uint64_t size);

uint64_t nvn_sparse_write(const void *pool, const void *src, uint64_t addr, uint64_t size);

static void storage_bytes(const tex_t *t, int sparse, size_t offset, uint8_t *linear, size_t n, int write) {
    if (write && sparse) nvn_sparse_write(t->pool,linear,(uintptr_t)t->mem+offset,n);
    else if (write) memcpy(t->mem + offset, linear, n);
    else if (sparse) nvn_sparse_copy(t->pool, linear, (uintptr_t)t->mem + offset, n);
    else memcpy(linear, t->mem + offset, n);
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

static int storage_texels(const tex_t *t, unsigned level, uint8_t *linear, size_t row, size_t img, texel_extent ext, int write) {
    if (!t->mem) return 0;
    size_t x0 = ext.x0, y0 = ext.y0, width = ext.bytes, rows = ext.rows;
    uint32_t nz = ext.nz, layer = ext.layer;
    int volume = ext.volume;
    int sparse = t->pool && nvn_sparse_pool(t->pool);
    if (write && !sparse) dm_guest_write_prepare(t->mem,t->L.size);
    if (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {
        size_t stride = t->L.layer_size / (t->d.height ? t->d.height : 1);
        if (level || layer || nz != 1 || x0 + width > stride) return 0;
        for (size_t y = 0; y < rows; ++y) storage_bytes(t, sparse, (y0 + y) * stride + x0, linear + y * row, width, write);
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
            storage_bytes(t, sparse, offset, linear + z * img + y * row + x, count, write);
            x += count;
        }
    }
    return 1;
}

// Vulkan transfers depth and stencil as separate buffer planes. Maxwell
// Z24S8 stores stencil in bits 0..7; ZF32_X24S8 stores float depth first.
// D24 can be backed by a D32F/S8 image on devices without native D24/S8.
static void packed_texel_row(const vtex *v, uint8_t *depth, uint8_t *stencil, const uint8_t *src, uint8_t *dst, size_t pixels) {
    if (!depth) {
        if (src) memcpy(stencil, src, pixels); else memcpy(dst, stencil, pixels);
        return;
    }
    int guest_float = v->nvn_fmt == NVN_FORMAT_DEPTH32F_STENCIL8;
    size_t bytes = guest_float ? 8 : 4;
    for (size_t x = 0; x < pixels; ++x) {
        if (src) {
            uint32_t word; memcpy(&word, src + bytes * x, 4);
            if (guest_float) memcpy(depth + 4 * x, &word, 4);
            else {
                word >>= 8;
                if (v->f.f == VK_FORMAT_D24_UNORM_S8_UINT) memcpy(depth + 4 * x, &word, 4);
                else { float z = (float)((double)word / 16777215.0); memcpy(depth + 4 * x, &z, 4); }
            }
            if (stencil) stencil[x] = src[bytes * x + (guest_float ? 4 : 0)];
        } else {
            uint32_t word; memcpy(&word, depth + 4 * x, 4);
            if (guest_float) {
                uint32_t s = stencil[x];
                memcpy(dst + bytes * x + 4, &s, 4);
            } else {
                if (v->f.f != VK_FORMAT_D24_UNORM_S8_UINT) {
                    float z; memcpy(&z, &word, 4);
                    word = (uint32_t)(fmin(1.0, fmax(0.0, z)) * 16777215.0 + 0.5);
                }
                word = (word << 8) | (stencil ? stencil[x] : 0);
            }
            memcpy(dst + bytes * x, &word, 4);
        }
    }
}

static void transfer_rows(const vtex *v, texel_extent ext, size_t pixels, const uint8_t *src, uint8_t *dst,
                          size_t row, size_t img, uint8_t *mapped, size_t depth_bytes, int stencil) {
    for (uint32_t z = 0; z < ext.nz; ++z) for (size_t y = 0; y < ext.rows; ++y) {
        size_t guest = z * img + y * row, line = z * ext.rows + y;
        if (v->f.stencil) {
            size_t first = line * pixels;
            packed_texel_row(v, depth_bytes ? mapped + 4 * first : NULL, stencil ? mapped + depth_bytes + first : NULL,
                             src ? src + guest : NULL, dst ? dst + guest : NULL, pixels);
        } else if (src) memcpy(mapped + line * ext.bytes, src + guest, ext.bytes);
        else memcpy(dst + guest, mapped + line * ext.bytes, ext.bytes);
    }
}

typedef struct {
    VkBufferImageCopy copies[2];
    uint32_t count;
    size_t bytes, depth_bytes;
    int stencil;
} texel_planes;

static texel_planes transfer_planes(const vtex *v, VkBufferImageCopy region, size_t native_bytes) {
    texel_planes p = {.copies={region},.count=1,.bytes=native_bytes};
    if (!v->f.stencil) return p;
    size_t pixels = (size_t)region.imageExtent.width * region.imageExtent.height *
                    region.imageExtent.depth * region.imageSubresource.layerCount;
    p.depth_bytes = v->nvn_fmt == NVN_FORMAT_STENCIL8 ? 0 : 4*pixels;
    p.stencil = v->nvn_fmt != NVN_FORMAT_DEPTH24;
    p.bytes = p.depth_bytes + (p.stencil ? pixels : 0);
    p.copies[0].imageSubresource.aspectMask = p.depth_bytes ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    if (p.depth_bytes && p.stencil) {
        p.count = 2;
        p.copies[1] = region;
        p.copies[1].bufferOffset += p.depth_bytes;
        p.copies[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return p;
}

// The public API may be called by a guest worker. One-time commands have
// their own command pool and use the same ordered VkQueue as submissions.
// Keep the image registry locked through completion so retirement cannot free it.
static int gpu_texels(const vtex *v, unsigned level, const NVNcopyRegion *r, const uint8_t *src, uint8_t *dst,
                      size_t row, size_t img, texel_extent ext) {
    if (v->f.dec || v->samples != VK_SAMPLE_COUNT_1_BIT) return 0;
    VkBufferImageCopy region = {0, 0, 0, {v->aspect, level, ext.volume ? 0 : ext.layer, ext.volume ? 1 : ext.nz},
                                {r->xoffset, r->yoffset, ext.volume ? (int32_t)ext.layer : 0},
                                {(uint32_t)r->width, (uint32_t)r->height, ext.volume ? ext.nz : 1}};
    texel_planes planes = transfer_planes(v, region, ext.bytes * ext.rows * ext.nz);
    VkDeviceMemory mem;
    void *mapped;
    VkBuffer buffer = buf_new(planes.bytes, src ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mem, &mapped);
    if (src) transfer_rows(v, ext, r->width, src, NULL, row, img, mapped, planes.depth_bytes, planes.stencil);
    VkCommandBuffer cb = once_begin();
    barrier_all(cb);
    if (src) {
        vkCmdCopyBufferToImage(cb, buffer, v->img, VK_IMAGE_LAYOUT_GENERAL, planes.count, planes.copies);
        barrier_all(cb);
    } else vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, buffer, planes.count, planes.copies);
    once_end(cb);
    if (dst) transfer_rows(v, ext, r->width, NULL, dst, row, img, mapped, planes.depth_bytes, planes.stencil);
    vkUnmapMemory(g_dev, mem);
    vkDestroyBuffer(g_dev, buffer, NULL);
    vkFreeMemory(g_dev, mem, NULL);
    return 1;
}

// Explicit CPU writes still update guest memory. An image the GPU already
// owns needs the same region on-device; re-uploading its whole backing would
// discard render-target contents outside the region.
void dm_vk_write_owned_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                             const uint8_t *src, size_t row, size_t img) {
    // An earlier linear-image writeback can still be publishing bytes into
    // guest backing after its GPU fence. It must finish before the caller's
    // CPU write to that backing, including writes through an aliased texture.
    gw_wait_prior((uintptr_t)t->mem, t->L.size);
    pthread_mutex_lock(&g_rmu);
    vtex *v = tex_find(t->serial);
    if (v && v->gpu) {
        texel_extent ext;
        if (!texel_region(t, level, layer0, r, row, img, &ext) || !gpu_texels(v, level, r, src, NULL, row, img, ext))
            RED_ONCE(81, "unsupported CPU write to GPU-owned texture (format %#x, samples %u)", t->d.format, t->d.samples);
    }
    pthread_mutex_unlock(&g_rmu);
}
