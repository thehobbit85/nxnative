// Use native reduction only when every min/max sampler in this stage can use
// it. Otherwise the existing shader variant supplies all reduction samplers.
// This keeps separate bindings and mixed sampler arrays on one coherent path.
static int native_reduction_view(const vtex *image,const treg_t *registration,int number,int multisample) {
    VkFormat format=dummy_format(number,multisample);
    if (image && registration->ok) {
        fmt_t viewed=view_format(image,&registration->v,registration->has_v);
        if (number==0) {
            NVNformat fit=float_word_format(viewed.f);
            if (fit!=NVN_FORMAT_NONE) viewed=fmt_of(fit);
        }
        format=depth_red_view(image,viewed) ? image->f.f : viewed.f;
        if (sampled_stencil(image,&registration->t,&registration->v,registration->has_v)) return 0;
        if (!g_minmax_swizzle) {
            uint8_t channels[]={2,3,4,5};
            sampled_swizzle(channels,viewed,&registration->t,&registration->v,registration->has_v);
            if (memcmp(channels,(uint8_t[]){2,3,4,5},sizeof channels)) return 0;
        }
    }
    VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(g_pd,format,&properties);
    return !!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT);
}
static int native_reduction_image(int stage,const sbind *binding,uint32_t element) {
    uint64_t handle=descriptor_handle(stage,binding,element);
    treg_t registration;
    vtex *image=registered_texture((int)(handle&0xfffff),&registration);
    return native_reduction_view(image,&registration,binding->num,binding->multisample);
}
static int native_shader_reduction(const shader_t *shader) {
    if (!g_minmax_filter) return 0;
    for (int i=0;i<shader->nb;i++) {
        const sbind *binding=&shader->b[i];
        if (binding->type!=BT_CIS && binding->type!=BT_SAMP) continue;
        int reduced=0;
        for (uint32_t element=0;element<binding->count;element++) {
            uint64_t handle=descriptor_handle(shader->guest_stage,binding,element);
            int id=(int)(handle>>20);
            pthread_mutex_lock(&g_rmu);
            samp_par sampler=id>0 && id<g_nsreg && g_sreg_ok[id] ? g_sreg[id] : (samp_par){0};
            pthread_mutex_unlock(&g_rmu);
            if (sampler.reduction!=NVN_SAMPLER_REDUCTION_MIN && sampler.reduction!=NVN_SAMPLER_REDUCTION_MAX) continue;
            // Vulkan's native min/max sampler forbids depth comparison.
            if (sampler.cmp_mode) return 0;
            if (fminf(16,fmaxf(1,floorf(sampler.aniso)))>g_max_anisotropy) return 0;
            reduced=1;
            if (binding->type==BT_CIS && !native_reduction_image(shader->guest_stage,binding,element)) return 0;
        }
        if (!reduced || binding->type!=BT_SAMP) continue;
        const sampler_pairs *pairs=shader->sampler_images ? &shader->sampler_images[i] : NULL;
        for (int texture=0;texture<shader->nb;texture++) {
            const sbind *image=&shader->b[texture];
            if (image->type!=BT_TEX && image->type!=BT_CIS) continue;
            if (pairs && !(pairs->mask[texture/64]&(UINT64_C(1)<<(texture%64)))) continue;
            for (uint32_t element=0;element<image->count;element++)
                if (!native_reduction_image(shader->guest_stage,image,element)) return 0;
        }
    }
    return 1;
}
