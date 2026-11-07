// The present thread records outside the guest command ring. Reuse the image
// copy shader, with a descriptor per fenced present and a PSO for the surface.
static VkPipelineLayout window_layout;
static int window_pipeline(void) {
    if (W.pipeline) return 1;
    static const uint32_t vert_code[] =
#include "clear.vert.inc"
    ;
    static const uint32_t frag_code[] =
#include "blit_color.frag.inc"
    ;
    static shader_t vs = {.key="present-vertex"};
    static shader_t fs = {.key="present-color", .stage=1, .nb=1, .b={{.type=BT_TEX,.count=1}}};
    if (!window_layout) {
        clear_shader(&vs, vert_code, sizeof vert_code);
        clear_shader(&fs, frag_code, sizeof frag_code);
        window_layout = image_draw_layout(&fs, sizeof(copy_draw_params));
        VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NPRES};
        VkDescriptorPoolCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, NPRES, 1, &size};
        VkDescriptorPool pool; VKCHECK(vkCreateDescriptorPool(g_dev, &info, NULL, &pool));
        VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &fs.dsl};
        for (int i = 0; i < NPRES; i++) VKCHECK(vkAllocateDescriptorSets(g_dev, &alloc, &g_pres[i].set));
    }
    pso_key key = {.vs=&vs, .fs=&fs, .prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .nc=1, .cf={W.fmt}, .chmask.mask=15};
    W.pipeline = pso_create(&key, window_layout);
    return W.pipeline != VK_NULL_HANDLE;
}

static void window_slot_retire(present_slot *slot) {
    if (slot->fb) vkDestroyFramebuffer(g_dev, slot->fb, NULL);
    if (slot->source) atomic_fetch_sub(&slot->source->present_refs, 1);
    slot->fb = VK_NULL_HANDLE;
    slot->source = NULL;
}

static int window_prepare(present_slot *slot, const tex_t *texture, const int crop[4], copy_draw_params *p) {
    pthread_mutex_lock(&g_rmu);
    vtex *v = tex_find(texture->serial);
    if (v) atomic_fetch_add(&v->present_refs, 1);
    pthread_mutex_unlock(&g_rmu);
    if (!v) return 0;
    slot->source = v;
    p->src[2] = v->w; p->src[3] = v->h;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0]+crop[2] > v->w || (uint64_t)crop[1]+crop[3] > v->h) {
            LOG("[RED] vk: present crop out of bounds: %d,%d %dx%d / source texture %ux%u", crop[0],crop[1],crop[2],crop[3],v->w,v->h);
            window_slot_retire(slot);
            return 0;
        }
        memcpy(p->src, crop, sizeof p->src);
    }
    if (g_origin_ll) { p->src[1] += p->src[3]; p->src[3] = -p->src[3]; }
    static int stretch = -1;
    if (stretch < 0) { const char *e = getenv("DM_SCREEN_FIT"); stretch = e && !strcmp(e, "stretch"); }
    vk_present_rect dst = vk_present_fit(atomic_load(&g_screen_shape), atomic_load(&g_layer_shape),
                                         v->w, v->h, W.ext.width, W.ext.height, stretch);
    p->dst[0] = dst.x; p->dst[1] = dst.y; p->dst[2] = dst.w; p->dst[3] = dst.h;
    VkImageView view = window_source(v);
    if (!view) { window_slot_retire(slot); return 0; }
    VkDescriptorImageInfo image = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, slot->set, 0, 0, 1,
                                  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image};
    vkUpdateDescriptorSets(g_dev, 1, &write, 0, NULL);
    return 1;
}
