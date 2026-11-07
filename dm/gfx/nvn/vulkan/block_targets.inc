// Android's sampled BC images contain decoded texels. An NVN encoder instead
// writes one uncompressed pixel per encoded block, as in Metal's bcrt_attach.
// The same shadow bridges D16 and D32F attachments with color-format views.
struct block_view {
    uint32_t level, layer;
    VkFormat format;
    vtex *shadow;
    block_view *next;
};
static void block_views_release(vtex *v) {
    block_view *entry=v->block_views;v->block_views=NULL;
    while (entry) {
        block_view *next=entry->next;
        tex_free(entry->shadow);free(entry);entry=next;
    }
}
static vtex *block_target_prepare(unsigned slot,const tex_t *texture,const view_t *view,int has_view) {
    int block_width, block_height, pixel_width, pixel_height;
    int bytes=nvn_fmt_bpb(texture->d.format,&block_width,&block_height);
    fmt_t original=fmt_of(texture->d.format);
    int depth=original.f==VK_FORMAT_D16_UNORM || original.f==VK_FORMAT_D32_SFLOAT;
    fmt_t format=has_view && view->set_format ? fmt_of(view->format) : (fmt_t){0};
    int pixel_bytes=has_view && view->set_format ? nvn_fmt_bpb(view->format,&pixel_width,&pixel_height) : 0;
    if (!format.f || format.dec || format.depth || pixel_bytes!=bytes || pixel_width!=1 || pixel_height!=1 ||
        (!depth && (block_width!=4 || block_height!=4 || !texture->mem)) || texture->d.samples>1) {
        RED_ONCE(122,"shadow attachment requires D16, D32F or BC storage and a block-sized color view");return NULL;
    }
    VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(g_pd,format.f,&properties);
    if (!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        RED_ONCE(122,"compressed attachment view format lacks color-attachment support");return NULL;
    }
    unsigned level=view->set_levels ? view->base_level : 0, layer=view->set_layers ? view->min_layer : 0;
    if (level>=32 || level>=(texture->d.levels ? texture->d.levels : 1)) return NULL;
    uint32_t width=texture->d.width>>level, height=texture->d.height>>level;
    if (!width) width=1;
    if (!height) height=1;
    uint32_t columns=(width+block_width-1)/block_width, rows=(height+block_height-1)/block_height;
    size_t row=(size_t)columns*bytes;
    int volume=texture->d.target==NVN_TEXTURE_TARGET_3D;
    NVNcopyRegion region={.zoffset=volume ? (int)layer : 0,.width=width,.height=height,.depth=1};
    texel_extent extent;
    if (!texel_region(texture,level,volume ? 0 : layer,&region,row,row*rows,&extent)) return NULL;
    if (!depth) cpu_ready((uintptr_t)texture->mem,texture->L.size);
    vtex *base=tex_get(texture,0);
    if (!base) return NULL;
    block_view *entry=base->block_views;
    while (entry && (entry->level!=level || entry->layer!=layer || entry->format!=format.f)) entry=entry->next;
    if (!entry) {
        vtex *shadow=sampled_scratch(format,VK_SAMPLE_COUNT_1_BIT,columns,rows,1,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!shadow) return NULL;
        entry=malloc(sizeof *entry);
        if (!entry) { tex_free(shadow);return NULL; }
        *entry=(block_view){level,layer,format.f,shadow,base->block_views};base->block_views=entry;
    }
    P.blocks[slot]=(block_target){.base=base,.texture=*texture,.extent=extent,.bytes=row*rows,
        .region={0,0,0,{depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,level,volume ? 0 : layer,1},
                 {0,0,volume ? (int)layer : 0},{width,height,1}}};
    return entry->shadow;
}
