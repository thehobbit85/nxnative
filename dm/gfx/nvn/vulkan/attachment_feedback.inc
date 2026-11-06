// Same-pass feedback: a draw that samples a colour attachment of its own pass. On the Switch it reads what was there before the draw; Vulkan leaves it undefined, and the Mali-G57
// returned garbage (Super Mario Odyssey's final pass, agl's post-effect program sampling the image it writes, drew yellow, green and red blotches over the picture; the Metal host
// needs the same for 3D World and Captain Toad, profile key feedback_snap). Here it holds for every game: the pass is split before such a draw, the attachment's level and layer
// copied into a snapshot image (reused between draws of the same format and size), and the draw samples the snapshot. Not done when the draw's colour mask leaves that attachment alone, nor again
// while no draw has written the attachment since its last snapshot. A 3D attachment is left as it was
typedef struct { vtex *image; uint64_t serial, last_use; uint32_t lv, ly; } fbsnap_t;
static fbsnap_t g_fbs[16]; static int g_nfbs;
static uint64_t g_fb_clock;
static fbsnap_t *g_fb_active[8];
static unsigned g_fb_mask;
static _Atomic uint64_t g_fb_copies, g_fb_reuses;

void mtl_fb_stats(uint64_t *copies, uint64_t *reuses, uint64_t *tile, uint64_t *wait) {
    *copies = g_fb_copies; *reuses = g_fb_reuses;
    *tile = 0; *wait = 0;
}

// Only a single sampled mip and layer that overlaps the active attachment
// needs substitution. Other mip/layer views read the original guest image.
static int fb_slot(vtex *v, const treg_t *tr, const sbind *b, unsigned mask) {
    if (!mask || v->target == NVN_TEXTURE_TARGET_3D) return -1;
    view_t vw = tr->has_v ? tr->v : (view_t){0};
    int target = vw.set_target ? vw.target : tr->t.d.target;
    int type = b->view;
    if (type < 0) {
        if (target == NVN_TEXTURE_TARGET_2D || target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || target == NVN_TEXTURE_TARGET_RECTANGLE) type = VK_IMAGE_VIEW_TYPE_2D;
        else if (target == NVN_TEXTURE_TARGET_2D_ARRAY || target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    if (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return -1;
    unsigned lv = vw.set_levels ? vw.base_level : 0, ly = vw.set_layers ? vw.min_layer : 0;
    if (lv >= v->levels || ly >= v->layers) return -1;
    if ((vw.set_levels && vw.nlevels ? vw.nlevels : v->levels - lv) != 1) return -1;
    if (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (vw.set_layers && vw.nlayers ? vw.nlayers : v->layers - ly) != 1) return -1;
    for (int a = 0; a < P.n; ++a) {
        if ((mask & (1u << a)) && P.ct[a] == v && P.clv[a] == lv && P.cly[a] == ly) return a;
    }
    return -1;
}
static fbsnap_t *fb_cached(int slot) {
    for (int k = 0; k < g_nfbs; ++k) {
        fbsnap_t *e = &g_fbs[k];
        if (e->serial == P.ct[slot]->serial && e->lv == P.clv[slot] && e->ly == P.cly[slot]) return e;
    }
    return NULL;
}
static vtex *sampled_scratch(fmt_t format, VkSampleCountFlagBits samples, uint32_t w, uint32_t h, uint32_t levels, VkImageUsageFlags usage) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        VK_IMAGE_TYPE_2D, format.f, {w,h,1}, levels, 1, samples, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(g_dev, &ii, NULL, &img) != VK_SUCCESS) { RED_ONCE(41, "sampled scratch image not created"); return NULL; }
    vtex *snap = calloc(1, sizeof *snap);
    if (!snap) die("Vulkan: allocating sampled scratch image");
    *snap = (vtex){.img=img,.w=w,.h=h,.d=1,.levels=levels,.layers=1,.target=NVN_TEXTURE_TARGET_2D,
        .f=format,.usage=ii.usage,.samples=samples,.aspect=VK_IMAGE_ASPECT_COLOR_BIT};
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, img, &mr);
    snap->blk = img_mem(mr, &snap->m, &snap->moff); snap->msize = mr.size;
    VKCHECK(vkBindImageMemory(g_dev, img, snap->m, snap->moff));
    to_general(C->cb, img, snap->aspect);
    return snap;
}
static vtex *sampled_snapshot(const vtex *v, uint32_t w, uint32_t h, uint32_t levels) {
    return sampled_scratch(v->f,v->samples,w,h,levels,VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}
static fbsnap_t *fb_snap_for(vtex *v, int i, fbsnap_t *e, unsigned reserved) {
    uint32_t lv = P.clv[i], ly = P.cly[i], w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1;
    if (e && !P.cwr[i]) { g_fb_reuses++; return e; }
    // Equal-shaped sources sampled by the same draw need distinct images.
    if (!e) for (int k = 0; k < g_nfbs; ++k) {
        if (!(reserved & (1u << k)) && g_fbs[k].image->f.f == v->f.f && g_fbs[k].image->samples == v->samples && g_fbs[k].image->w == w && g_fbs[k].image->h == h) {
            e = &g_fbs[k]; break;
        }
    }
    if (!e) {
        vtex *snap = sampled_snapshot(v, w, h, 1);
        if (!snap) return NULL;
        if (g_nfbs < (int)(sizeof g_fbs / sizeof *g_fbs)) e = &g_fbs[g_nfbs++];
        else {
            // At most eight entries are reserved by this draw. Descriptors
            // already recorded can still reference the retired image's views.
            for (int k = 0; k < g_nfbs; ++k)
                if (!(reserved & (1u << k)) && (!e || g_fbs[k].last_use < e->last_use)) e = &g_fbs[k];
            tex_free(e->image);
        }
        e->image = snap;
        LOG("vk: same-pass feedback: snapshot %ux%u format %d made (serial %llu)", w, h, (int)v->f.f, (unsigned long long)v->serial);
    }
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, e->image->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    e->serial = v->serial; e->lv = lv; e->ly = ly; P.cwr[i] = 0; g_fb_copies++;
    return e;
}
static void fb_prepare(shader_t *fs) {   // before a draw (pass open): find the sampled colour attachments it writes, snapshot them, and have write_set bind the snapshots
    g_fb_mask = 0;
    unsigned writable = 0, want = 0;
    for (int a = 0; a < P.n; ++a) if (P.cv[a] && (S.chmask.mask >> (4*a) & 0xf)) writable |= 1u << a;
    if (!writable) return;
    for (int i = 0; i < fs->nb; i++) {
        const sbind *b = &fs->b[i];
        if ((b->type != BT_CIS && b->type != BT_TEX) || (b->kind != K_TEX && b->kind != K_TEXU) || (!b->table && b->count > 32)) continue;
        for (uint32_t element = 0; element < b->count; ++element) {
            int tid = (int)(descriptor_handle(1, b, element) & 0xfffff);
            if (!tid) continue;
            treg_t tr; vtex *v = registered_texture(tid, &tr);
            int a = v ? fb_slot(v, &tr, b, writable) : -1;
            if (a >= 0) want |= 1u << a;
        }
    }
    if (!want) return;
    unsigned reserved = 0;
    int need = 0;
    fbsnap_t *cached[8] = {0};
    for (int a = 0; a < P.n; ++a) if (want & (1u << a)) {
        cached[a] = fb_cached(a);
        if (cached[a]) reserved |= 1u << (cached[a] - g_fbs);
        if (!cached[a] || P.cwr[a]) need = 1;
    }
    VkCommandBuffer cb = C->cb;
    if (need) {
        pass_end();
        barrier_all(cb);   // earlier fragment reads must finish before recycling their snapshot
    }
    for (int a = 0; a < P.n; ++a) if (want & (1u << a)) {
        fbsnap_t *e = fb_snap_for(P.ct[a], a, cached[a], reserved);
        if (!e) continue;
        e->last_use = ++g_fb_clock;
        reserved |= 1u << (e - g_fbs);
        g_fb_active[a] = e;
        g_fb_mask |= 1u << a;
    }
    if (need) { barrier_all(cb); pass_begin(); }
}
