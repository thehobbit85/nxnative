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
        for (uint32_t element=0;element<binding->count;element++) {
            int id=(int)(descriptor_handle(base->guest_stage,binding,element)&0xfffff);
            pthread_mutex_lock(&g_rmu);
            int buffer=id>0 && id<g_ntreg && g_treg[id].ok && g_treg[id].t.d.target==NVN_TEXTURE_TARGET_BUFFER;
            pthread_mutex_unlock(&g_rmu);
            if (buffer) {
                if (binding->count!=1) {
                    RED_ONCE(24,"wrapped buffer arrays require per-element image interfaces");return 0;
                }
                wrapped[i]=1;any=1;
            }
        }
    }
    if (!any) return 1;
    for (shader_t *variant=base->wrapped_variants;variant;variant=variant->next_wrapped_variant)
        if (!memcmp(variant->wrapped,wrapped,sizeof wrapped)) {
            *selected=variant;return variant->mod && variant->dsl;
        }
    shader_t *variant=malloc(sizeof *variant);
    if (!variant) return 0;
    *variant=*base;
    variant->buffer_variants=NULL;variant->wrapped_variants=NULL;variant->vertex_variants=NULL;
    variant->fragment_variants=NULL;variant->cpp=VK_NULL_HANDLE;variant->cpl=VK_NULL_HANDLE;
    variant->mod=VK_NULL_HANDLE;variant->dsl=VK_NULL_HANDLE;
    memcpy(variant->wrapped,wrapped,sizeof wrapped);
    for (int i=0;i<variant->nb;i++) if (wrapped[i]) variant->b[i].type=BT_UTB;
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
