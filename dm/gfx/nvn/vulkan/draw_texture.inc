// The rectangle is in attachment pixels in both window origins. Clip to
// pixel centers without changing the original source-to-destination mapping.
static VkRect2D draw_texture_rect(const NVNdrawTextureRegion *d, VkExtent2D size) {
    double left = fmax(0, fmin(d->x0, d->x1)), right = fmin(size.width, fmax(d->x0, d->x1));
    double top = fmax(0, fmin(d->y0, d->y1)), bottom = fmin(size.height, fmax(d->y0, d->y1));
    if (S.has_sc) {
        left = fmax(left, S.sc.x); right = fmin(right, (double)S.sc.x + S.sc.w);
        top = fmax(top, S.sc.y); bottom = fmin(bottom, (double)S.sc.y + S.sc.h);
    }
    if (!(left < right && top < bottom)) return (VkRect2D){0};
    int x = (int)ceil(left - .5), y = (int)ceil(top - .5);
    return (VkRect2D){{x,y},{(uint32_t)((int)ceil(right - .5) - x),(uint32_t)((int)ceil(bottom - .5) - y)}};
}

// An MRT draw can also sample one of its destinations. Preserve every mip
// visible through the registered 2D view before touching the first target.
static VkImageView draw_texture_source(vtex *v, const treg_t *tr) {
    view_t vw = tr->has_v ? tr->v : (view_t){0};
    uint32_t base = vw.set_levels ? vw.base_level : 0, layer = vw.set_layers ? vw.min_layer : 0;
    if (base >= v->levels || layer >= v->layers) return VK_NULL_HANDLE;
    uint32_t levels = v->levels - base;
    if (vw.set_levels && vw.nlevels && vw.nlevels < levels) levels = vw.nlevels;
    for (int i = 0; i < P.n; ++i) {
        if (P.ct[i] != v || P.cly[i] != layer || P.clv[i] - base >= levels || !(S.chmask.mask >> (4*i) & 15)) continue;
        uint32_t w = v->w >> base ? v->w >> base : 1, h = v->h >> base ? v->h >> base : 1;
        vtex *snapshot = sampled_snapshot(v, w, h, levels);
        if (!snapshot) return VK_NULL_HANDLE;
        for (uint32_t l = 0; l < levels; ++l) {
            VkImageCopy copy = {{v->aspect,base+l,layer,1},{0,0,0},{v->aspect,l,0,1},{0,0,0},{w,h,1}};
            vkCmdCopyImage(C->cb,v->img,VK_IMAGE_LAYOUT_GENERAL,snapshot->img,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
        }
        barrier_all(C->cb);
        vw.set_levels = vw.set_layers = 1; vw.base_level = vw.min_layer = 0; vw.nlevels = levels; vw.nlayers = 1;
        VkImageView view = tex_view(snapshot,&tr->t,&vw,1,VK_IMAGE_VIEW_TYPE_2D,VIEW_SAMPLED);
        tex_free(snapshot); // deferred until all commands using its view have completed
        return view;
    }
    return tex_view(v,&tr->t,&vw,tr->has_v,VK_IMAGE_VIEW_TYPE_2D,VIEW_SAMPLED);
}

static void draw_texture(const p_draw_tex *c) {
    static const uint32_t code[] =
#include "draw_texture.frag.inc"
    ;
    static shader_t fragment = {.key="draw-texture",.stage=1,.nb=1,.b={{.type=BT_CIS,.count=1}}};
    static VkPipelineLayout layout;
    float dw = c->d.x1 - c->d.x0, dh = c->d.y1 - c->d.y0;
    if (!P.n || !dw || !dh) return;
    pass_end(); // pending clears and previous guest writes must land first
    treg_t tr;
    vtex *v = registered_texture((int)(c->h & 0xfffff), &tr);
    int sid = (int)(c->h >> 20);
    pthread_mutex_lock(&g_rmu);
    VkSampler sampler = sid > 0 && sid < g_nsreg && g_sreg_ok[sid] ? samp_get(sid) : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_rmu);
    if (!v || !sampler) { RED_ONCE(102, "DrawTexture handle has no texture or sampler"); return; }
    int type = tr.has_v && tr.v.set_target ? tr.v.target : tr.t.d.target;
    fmt_t format = view_format(v,&tr.v,tr.has_v);
    if ((type != NVN_TEXTURE_TARGET_2D && type != NVN_TEXTURE_TARGET_RECTANGLE) ||
        v->samples != VK_SAMPLE_COUNT_1_BIT || format.depth || clear_color_type(format.f) != CLEAR_FLOAT) {
        RED_ONCE(102, "DrawTexture requires a single-sample float or normalized 2D view"); return;
    }
    barrier_all(C->cb);
    VkImageView source = draw_texture_source(v,&tr);
    if (!source) return;
    float params[] = {c->d.x0,c->d.y0,c->s.x0,c->s.y0,(c->s.x1-c->s.x0)/dw,(c->s.y1-c->s.y0)/dh};
    if (!layout) layout = image_draw_layout(&fragment,sizeof params);
    clear_shader(&fragment,code,sizeof code);
    VkDescriptorSet set = image_draw_set(&fragment,source,sampler);
    if (S.color.blend_en) RED_ONCE(102, "DrawTexture with blending enabled draws without blending, as on Metal");
    for (int i = 0; i < P.n; ++i) {
        unsigned mask = S.chmask.mask >> (4*i) & 15;
        if (!P.cv[i] || !mask) continue;
        if (clear_color_type(P.cf[i]) != CLEAR_FLOAT || P.ct[i]->f.dec) {
            RED_ONCE(102, "DrawTexture requires a float or normalized color target"); continue;
        }
        vtex *dst = P.ct[i];
        uint32_t level = P.clv[i];
        VkExtent2D size = {dst->w >> level ? dst->w >> level : 1,dst->h >> level ? dst->h >> level : 1};
        VkRect2D rect = draw_texture_rect(&c->d,size);
        if (!rect.extent.width || !rect.extent.height) continue;
        pso_key k = {.vs=clear_vertex(),.fs=&fragment,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.nc=1,
            .cf={P.cf[i]},.samples=dst->samples,.chmask={.mask=mask}};
        VkPipeline pipeline = pso_get(&k,layout);
        if (!pipeline) continue;
        rp_desc pass = {.n=1,.w=size.width,.h=size.height,.samples=dst->samples,.cv={P.cv[i]},.cf={P.cf[i]},
            .cl={VK_ATTACHMENT_LOAD_OP_LOAD},.layout=VK_IMAGE_LAYOUT_GENERAL};
        begin_tracked_pass(&pass);
        clear_draw_state(pipeline,rect,size,0);
        vkCmdBindDescriptorSets(C->cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);
        vkCmdPushConstants(C->cb,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof params,params);
        vkCmdDraw(C->cb,3,1,0,0);
        rp_end(C->cb);
        barrier_all(C->cb);
        P.cwr[i] = 1;
        if (dst->linrow && !level && !P.cly[i]) { lin_writeback(dst); barrier_all(C->cb); }
    }
}
