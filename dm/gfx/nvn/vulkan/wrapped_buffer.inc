// Select the descriptor representation before collecting shared storage ranges.
// Buffer aliases then use the same ring allocation as writable SSBO bindings.
static int prepare_wrapped_buffers(shader_t **selected) {
    shader_t *base=*selected;
    if (!base) return 1;
    uint8_t wrapped[DM_VK_MAX_BINDINGS]={0};int any=0;
    for (int i=0;i<base->nb;i++) {
        const sbind *binding=&base->b[i];
        if ((binding->type!=BT_CIS && binding->type!=BT_TEX) || binding->view!=VK_IMAGE_VIEW_TYPE_2D ||
            (!binding->shape.fetch1d && binding->kind!=K_TEXU)) continue;
        int has_buffer=0, has_image=0;
        for (uint32_t element=0;element<binding->count;element++) {
            int id=(int)(descriptor_handle(base->guest_stage,binding,element)&0xfffff);
            pthread_mutex_lock(&g_rmu);
            if (id>0 && id<g_ntreg && g_treg[id].ok) {
                if (g_treg[id].t.d.target==NVN_TEXTURE_TARGET_BUFFER) has_buffer=1;
                else has_image=1;
            }
            pthread_mutex_unlock(&g_rmu);
        }
        if (has_buffer && has_image && (binding->table || !binding->count || binding->count>32)) {
            RED_ONCE(24,"mixed wrapped descriptors require a fixed array of at most 32 elements");return 0;
        }
        if (has_buffer && binding->table && !g_texel_descriptor_tables) {
            RED_ONCE(24,"wrapped handle tables require nonuniform texel-buffer descriptor indexing");return 0;
        }
        if (has_buffer) { wrapped[i]=has_image ? 2 : 1;any=1; }
    }
    if (!any) return 1;
    for (shader_t *variant=base->wrapped_variants;variant;variant=variant->next_wrapped_variant)
        if (!memcmp(variant->wrapped,wrapped,(size_t)base->nb)) {
            *selected=variant;return variant->mod && variant->dsl;
        }
    shader_t *variant=malloc(sizeof *variant);
    if (!variant) return 0;
    *variant=*base;
    variant->buffer_variants=NULL;variant->wrapped_variants=NULL;variant->vertex_variants=NULL;
    variant->fragment_variants=NULL;variant->cpp=VK_NULL_HANDLE;variant->cpl=VK_NULL_HANDLE;
    variant->mod=VK_NULL_HANDLE;variant->dsl=VK_NULL_HANDLE;
    memcpy(variant->wrapped,wrapped,sizeof wrapped);
    uint32_t binding=0;
    for (int i=0;i<base->nb;i++) if (base->b[i].binding>=binding) binding=base->b[i].binding+1;
    for (int i=0;i<base->nb;i++) {
        if (wrapped[i]==1) variant->b[i].type=BT_UTB;
        if (wrapped[i]!=2) continue;
        if (variant->nb>DM_VK_MAX_BINDINGS-2 || binding>UINT32_MAX-2) {
            RED_ONCE(24,"mixed descriptor interfaces exceed the shader binding capacity");free(variant);return 0;
        }
        sbind *image=&variant->b[i],*buffer=&variant->b[variant->nb];
        *buffer=*image;buffer->binding=binding++;buffer->type=BT_UTB;
        image->mixed_image=1;
        variant->wrapped[variant->nb++]=1;
        sbind *flags=&variant->b[variant->nb++];
        *flags=(sbind){.binding=binding++,.type=BT_SSBO,.count=1,.kind=K_WRAP_FLAGS,.reg=i,.read_only=1};
        variant->mixed[variant->nmixed++]=(mixed_wrapped_edit){image->binding,buffer->binding,flags->binding,image->count};
    }
    variant->next_wrapped_variant=base->wrapped_variants;base->wrapped_variants=variant;
    char path[1024];size_t bytes;
    snprintf(path,sizeof path,"%s/%s.spv",g_vkdir,variant->key);
    uint32_t *words=read_file(path,&bytes);
    if (!words) return 0;
    words=stage_spv(words,&bytes,variant);
    if (!words) return 0;
    VkShaderModuleCreateInfo create={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,bytes,words};
    VkResult result=vkCreateShaderModule(g_dev,&create,NULL,&variant->mod);free(words);
    if (result!=VK_SUCCESS) { variant->mod=VK_NULL_HANDLE;return 0; }
    variant->dsl=shader_layout(variant);
    *selected=variant;
    return variant->dsl!=VK_NULL_HANDLE;
}
