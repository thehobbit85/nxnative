// Select portable correction only for scalar ordinary float samplers. Keep
// native custom colors for compatible views while the driver has capacity.
static int border_image_view(const shader_t *shader,const sbind *binding,border_view *out) {
    if (binding->num || binding->multisample || binding->depth_image || binding->mixed_image || binding->count!=1) return 0;
    uint64_t handle=descriptor_handle(shader->guest_stage,binding,0);
    treg_t registration;vtex *image=registered_texture((int)(handle&0xfffff),&registration);
    if (!image || !registration.ok) return 0;
    fmt_t format=view_format(image,&registration.v,registration.has_v);
    if (format.depth || clear_color_type(format.f)!=CLEAR_FLOAT) return 0;
    *out=sampler_view(image,&registration);
    return !(out->flags&BORDER_SWIZZLED);
}
static int prepare_custom_borders(shader_t **selected) {
    shader_t *base=*selected;if (!base) return 1;
    sample_border borders[DM_VK_MAX_BINDINGS]={0};size_t count=0;
    uint32_t next_binding=0;
    for (int i=0;i<base->nb;i++) if (base->b[i].binding>=next_binding) next_binding=base->b[i].binding+1;
    for (int i=0;i<base->nb;i++) {
        const sbind *binding=&base->b[i];
        if ((binding->type!=BT_CIS && binding->type!=BT_SAMP) || binding->count!=1) continue;
        uint64_t handle=descriptor_handle(base->guest_stage,binding,0);int id=(int)(handle>>20);
        pthread_mutex_lock(&g_rmu);
        samp_par sampler=id>0 && id<g_nsreg && g_sreg_ok[id] ? g_sreg[id] : (samp_par){0};
        pthread_mutex_unlock(&g_rmu);
        int nearest,other;float factors[4];const uint8_t wrap[]={sampler.ws,sampler.wt,sampler.wr};
        if (sampler.cmp_mode || !nvn_border(sampler.border,wrap,sampler.reduction,&nearest,&other,factors)) continue;
        int compatible=1,found=0,native=g_custom_border_count<g_custom_border_limit;
        border_view first={0};
        const sampler_pairs *pairs=base->sampler_images ? &base->sampler_images[i] : NULL;
        for (int j=0;j<base->nb;j++) {
            const sbind *image=&base->b[j];
            if (binding->type==BT_CIS ? j!=i : image->type!=BT_TEX ||
                (pairs && !(pairs->mask[j/64]&(UINT64_C(1)<<(j%64))))) continue;
            border_view view;
            if (!border_image_view(base,image,&view)) { compatible=0;break; }
            if (found && first.format!=view.format) native=0;
            first=view;found=1;
        }
        if (!compatible || !found) continue;
        if (native) {
            // Materialize it now so another binding cannot consume the last
            // custom-color slot before this descriptor is written.
            pthread_mutex_lock(&g_rmu);
            native=g_custom_border_count<g_custom_border_limit;
            if (native) (void)sampler_for(sampler,first);
            pthread_mutex_unlock(&g_rmu);
            if (native) continue;
        }
        if (base->nb+(int)count>=DM_VK_MAX_BINDINGS || next_binding==UINT32_MAX) return 0;
        sample_border *edit=&borders[count++];
        *edit=(sample_border){.binding=binding->binding,.twin_binding=next_binding++,.nearest=nearest,.other=other};
        memcpy(edit->factor,factors,sizeof factors);
    }
    if (!count) return 1;
    for (shader_t *s=base->border_variants;s;s=s->next_border_variant)
        if (s->nborders==count && !memcmp(s->borders,borders,count*sizeof *borders)) {
            *selected=s;return s->mod && s->dsl;
        }
    shader_t *s=malloc(sizeof *s);if (!s) return 0;
    *s=*base;s->border_variants=NULL;s->buffer_variants=NULL;s->wrapped_variants=NULL;
    s->vertex_variants=NULL;s->fragment_variants=NULL;s->cpp=VK_NULL_HANDLE;s->cpl=VK_NULL_HANDLE;
    s->mod=VK_NULL_HANDLE;s->dsl=VK_NULL_HANDLE;
    s->nborders=count;memcpy(s->borders,borders,count*sizeof *borders);
    if (base->sampler_images) {
        s->sampler_images=calloc(DM_VK_MAX_BINDINGS,sizeof *s->sampler_images);
        if (!s->sampler_images) { free(s);return 0; }
        memcpy(s->sampler_images,base->sampler_images,(size_t)base->nb*sizeof *s->sampler_images);
    }
    for (size_t e=0;e<count;e++) for (int i=0;i<base->nb;i++) if (s->b[i].binding==borders[e].binding) {
        sbind *original=&s->b[i],*twin=&s->b[s->nb];
        *twin=*original;twin->binding=borders[e].twin_binding;twin->logical_count=0;
        original->border_stock=borders[e].nearest+1;twin->border_stock=borders[e].other+1;
        if (s->sampler_images) s->sampler_images[s->nb]=s->sampler_images[i];
        s->nb++;break;
    }
    s->next_border_variant=base->border_variants;base->border_variants=s;
    char path[1024];size_t bytes;snprintf(path,sizeof path,"%s/%s.spv",g_vkdir,s->key);
    uint32_t *words=read_file(path,&bytes);if (!words) return 0;
    words=stage_spv(words,&bytes,s);if (!words) return 0;
    VkShaderModuleCreateInfo create={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,bytes,words};
    VkResult result=vkCreateShaderModule(g_dev,&create,NULL,&s->mod);free(words);
    if (result!=VK_SUCCESS) { s->mod=VK_NULL_HANDLE;return 0; }
    s->dsl=shader_layout(s);*selected=s;return s->dsl!=VK_NULL_HANDLE;
}
