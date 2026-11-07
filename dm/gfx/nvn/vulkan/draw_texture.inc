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

typedef struct {
    float dst[2], src[2], scale[2];
    int32_t reduction, linear, level0, level1;
    float span[2];
    int32_t taps;
    float padding[3], border[4];
    int32_t border_emu;
} draw_texture_params;
_Static_assert(offsetof(draw_texture_params,border)==64,"DrawTexture border push constant alignment");

// Reuse Metal's two-stock-color identity when native custom borders are absent
// or exhausted. Weighted filtering is linear in the border color, so the two
// filtered values reconstruct it without changing the interior texels.
static VkSampler draw_texture_average_sampler(samp_par p,border_view sampled,draw_texture_params *params,VkSampler *twin) {
    int native=sampled.format!=VK_FORMAT_UNDEFINED && !(sampled.flags&BORDER_SWIZZLED) &&
        g_custom_border_count<g_custom_border_limit;
    int nearest,other;float factors[4];const uint8_t wrap[]={p.ws,p.wt,p.wr};
    if (native || p.cmp_mode || (sampled.flags&BORDER_SWIZZLED) || !nvn_border(p.border,wrap,0,&nearest,&other,factors))
        return sampler_for(p,sampled);
    memcpy(p.border,nvn_mtl_border[nearest],sizeof p.border);
    VkSampler sampler=sampler_for(p,sampled);
    memcpy(p.border,nvn_mtl_border[other],sizeof p.border);
    *twin=sampler_for(p,sampled);
    memcpy(params->border,factors,sizeof factors);params->border_emu=1;
    return sampler;
}

// The rectangle's texel-to-pixel ratio is constant. Select its mip footprint
// once and let the shader combine the actual texels before any averaging.
static VkSampler draw_texture_sampler(samp_par p, const vtex *v, const treg_t *tr, draw_texture_params *params,VkSampler *twin) {
    border_view sampled = sampler_view(v,tr);
    if (p.reduction != NVN_SAMPLER_REDUCTION_MIN && p.reduction != NVN_SAMPLER_REDUCTION_MAX)
        return draw_texture_average_sampler(p,sampled,params,twin);
    float anisotropy=fminf(16,fmaxf(1,floorf(p.aniso)));
    if (g_minmax_filter && !p.cmp_mode && anisotropy<=g_max_anisotropy && native_reduction_view(v,tr,0,0)) return sampler_for(p,sampled);
    if (p.cmp_mode) {
        RED_ONCE(103, "DrawTexture reduction with depth comparison is unavailable");
        p.reduction=NVN_SAMPLER_REDUCTION_AVERAGE;
        return sampler_for(p,sampled);
    }
    uint32_t levels = v->levels;
    if (tr->has_v && tr->v.set_levels) {
        if (tr->v.base_level >= levels) return VK_NULL_HANDLE;
        levels -= tr->v.base_level;
        if (tr->v.nlevels && tr->v.nlevels < levels) levels = tr->v.nlevels;
    }
    // A rectangle has axis-aligned derivatives. Spread point-filtered bilinear
    // footprints along its major axis and select mips from their shorter scale.
    // Anisotropic kernels vary between GPUs; this follows Vulkan's N-isotropic-
    // sample model and works without the native samplerAnisotropy feature.
    float x=fabsf(params->scale[0]),y=fabsf(params->scale[1]);
    float major=fmaxf(x,y),minor=fminf(x,y);
    params->taps=(int)fmaxf(1,fminf(anisotropy,ceilf(major/fmaxf(1,minor))));
    params->span[x>=y ? 0 : 1]=params->scale[x>=y ? 0 : 1];
    float rho=fmaxf(minor,major/params->taps);
    float lod = log2f(rho) + fminf(15.99f, fmaxf(-16, p.lod_bias));
    float lo=p.minf>=2 ? fmaxf(0,p.lod_min) : 0,hi=p.minf>=2 ? fmaxf(lo,p.lod_max) : 0;
    lod=fminf(hi,fmaxf(lo,lod));
    params->reduction = p.reduction;
    params->linear = lod > 0 ? (p.minf & 1) : p.magf;
    lod=fminf(levels-1,fmaxf(0,lod));
    // The Metal reference forces linear mip reduction even when the guest's
    // minification filter selected a nearest mip. Non-mipped samplers clamp 0.
    params->level0=(int)floorf(lod);params->level1=(int)ceilf(lod);
    // Point samples retain the registered sampler's addressing and border.
    p.minf = NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST; p.magf = NVN_MAG_FILTER_NEAREST;
    p.reduction = NVN_SAMPLER_REDUCTION_AVERAGE; p.lod_bias = 0;
    p.lod_min = 0; p.lod_max = VK_LOD_CLAMP_NONE; p.aniso=1;
    return sampler_for(p,sampled);
}

static void draw_texture(const p_draw_tex *c) {
    static const uint32_t code[] =
#include "draw_texture.frag.inc"
    ;
    static shader_t fragment = {.key="draw-texture",.stage=1,.nb=2,.b={{.type=BT_CIS,.count=1},{.binding=1,.type=BT_CIS,.count=1}}};
    static VkPipelineLayout layout;
    float dw = c->d.x1 - c->d.x0, dh = c->d.y1 - c->d.y0;
    if (!P.n || !dw || !dh) return;
    pass_end(); // pending clears and previous guest writes must land first
    treg_t tr;
    vtex *v = registered_texture((int)(c->h & 0xfffff), &tr);
    int sid = (int)(c->h >> 20);
    draw_texture_params params = {{c->d.x0,c->d.y0}, {c->s.x0,c->s.y0}, {(c->s.x1-c->s.x0)/dw,(c->s.y1-c->s.y0)/dh}};
    VkSampler twin=VK_NULL_HANDLE;
    pthread_mutex_lock(&g_rmu);
    VkSampler sampler = v && sid > 0 && sid < g_nsreg && g_sreg_ok[sid] ? draw_texture_sampler(g_sreg[sid],v,&tr,&params,&twin) : VK_NULL_HANDLE;
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
    if (!layout) layout = image_draw_layout(&fragment,sizeof params);
    clear_shader(&fragment,code,sizeof code);
    VkDescriptorSet set=allocate_shader_set(&fragment);
    VkDescriptorImageInfo images[]={{sampler,source,VK_IMAGE_LAYOUT_GENERAL},{twin ? twin : sampler,source,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2];
    for (int binding=0;binding<2;binding++) writes[binding]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        NULL,set,binding,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,images+binding};
    vkUpdateDescriptorSets(g_dev,2,writes,0,NULL);
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
        vkCmdPushConstants(C->cb,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof params,&params);
        vkCmdDraw(C->cb,3,1,0,0);
        rp_end(C->cb);
        barrier_all(C->cb);
        P.cwr[i] = 1;
        if (dst->linrow && !level && !P.cly[i]) { lin_writeback(dst); barrier_all(C->cb); }
    }
}
