// Use the exported .vc vertex entry during the ordinary draw. It preserves
// rasterization and the guest's storage side effects, and emits one unpacked
// record per vertex/instance. A compute pass packs those records for NVN.
static shader_t *feedback_capture_shader(shader_t *vs) {
    if (!vs->capture_variant) {
        char key[32];
        snprintf(key,sizeof key,"%.28s.vc",vs->key);
        vs->capture_variant=shader_by_key(key);
    }
    return vs->capture_variant;
}
static shader_t *feedback_vertex(shader_t *vs, const p_draw *draw, capture_draw *capture) {
    if (!draw || draw->instances < 0 || (draw->prim != NVN_DRAW_PRIMITIVE_POINTS &&
        draw->prim != NVN_DRAW_PRIMITIVE_LINES && draw->prim != NVN_DRAW_PRIMITIVE_TRIANGLES) ||
        S.bst[2] || S.bst[3] || S.bst[4]) {
        RED_ONCE(101, "transform feedback requires a vertex point, line or triangle list"); return vs;
    }
    if (!g_vertex_stores || !S.feedback) {
        RED_ONCE(101, "transform feedback needs vertex storage and initialized controls"); return vs;
    }
    if (draw->count <= 0) return vs;
    unsigned instances = draw->instances ? (unsigned)draw->instances : 1;
    uint64_t count = (uint64_t)draw->count * instances, capacity = count;
    for (unsigned b=0; b<4; b++) if (vs->feedback_layout->stride[b]) {
        const p_tf_bind *range = &S.feedback_streams[b];
        if (!range->addr || range->size > g_props.limits.maxStorageBufferRange ||
            !mtl_addr(range->addr,range->size,NULL,NULL)) {
            RED_ONCE(102, "transform feedback output is unbound or exceeds the device storage limit"); return vs;
        }
        uint64_t records = range->size / vs->feedback_layout->stride[b];
        if (records < capacity) capacity = records;
    }
    if (!capacity) return vs;
    if (count > g_props.limits.maxStorageBufferRange / (144*4) ||
        (capacity+63)/64 > g_props.limits.maxComputeWorkGroupCount[0]) {
        RED_ONCE(102, "transform feedback scratch exceeds the device capture range"); return vs;
    }
    shader_t *entry = feedback_capture_shader(vs);
    if (!entry->mod || !entry->capture_entry || !entry->feedback_layout ||
        memcmp(entry->feedback_layout,vs->feedback_layout,sizeof *vs->feedback_layout)) {
        RED_ONCE(100, "missing transform feedback capture entry for %s", vs->key); return vs;
    }
    // The capture entry guards its entire vertex body. Every guest invocation
    // must fit here for rasterization and atomics, even when a stream is full.
    VkDeviceSize size = count * 144 * 4;
    VkDeviceSize offset = ring_alloc(size,g_props.limits.minStorageBufferOffsetAlignment);
    capture->records = (VkDescriptorBufferInfo){C->ring,offset,size};
    uint32_t control[] = {(uint32_t)draw->first,(uint32_t)draw->count,(uint32_t)draw->base_instance,instances,0,(uint32_t)count};
    offset = ring_put(control,sizeof control,g_props.limits.minUniformBufferOffsetAlignment);
    capture->control = (VkDescriptorBufferInfo){C->ring,offset,sizeof control};
    capture->layout = vs->feedback_layout;
    capture->capacity = (uint32_t)capacity;
    capture->primitive = draw->prim == NVN_DRAW_PRIMITIVE_POINTS ? 1 : draw->prim == NVN_DRAW_PRIMITIVE_LINES ? 2 : 3;
    return entry;
}

static shader_t *clear_shader(shader_t *shader, const uint32_t *code, size_t bytes);
static void feedback_dispatch(shader_t *shader, const uint32_t *code, size_t bytes, const VkDescriptorBufferInfo *buffers,
                              const uint32_t *push, uint32_t push_bytes, uint32_t groups) {
    if (!shader->cpp) {
        clear_shader(shader,code,bytes);
        shader->dsl=shader_layout(shader);
        VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,push_bytes};
        VkPipelineLayoutCreateInfo layout={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,1,&shader->dsl,
                                            push_bytes ? 1u : 0u,push_bytes ? &range : NULL};
        VKCHECK(vkCreatePipelineLayout(g_dev,&layout,NULL,&shader->cpl));
        VkComputePipelineCreateInfo pipeline={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,NULL,0,
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_COMPUTE_BIT,shader->mod,"main",NULL},shader->cpl};
        VKCHECK(vkCreateComputePipelines(g_dev,g_pcache,1,&pipeline,NULL,&shader->cpp));
    }
    VkDescriptorSet set=allocate_shader_set(shader);
    VkWriteDescriptorSet writes[4];
    for (int i=0;i<shader->nb;i++) writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,set,
        shader->b[i].binding,0,1,descriptor_type[shader->b[i].type],NULL,&buffers[i],NULL};
    vkUpdateDescriptorSets(g_dev,shader->nb,writes,0,NULL);
    vkCmdBindPipeline(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,shader->cpp);
    vkCmdBindDescriptorSets(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,shader->cpl,0,1,&set,0,NULL);
    if (push_bytes) vkCmdPushConstants(C->cb,shader->cpl,VK_SHADER_STAGE_COMPUTE_BIT,0,push_bytes,push);
    vkCmdDispatch(C->cb,groups,1,1);
}

static void feedback_finish(const capture_draw *capture) {
    static const uint32_t prepare_code[] =
#include "feedback_prepare.comp.inc"
    ;
    static const uint32_t gather_code[] =
#include "feedback_gather.comp.inc"
    ;
    static shader_t prepare = {.stage=5,.nb=2,.b={{.type=BT_SSBO,.count=1},{.binding=1,.type=BT_SSBO,.count=1}}};
    static shader_t gather = {.stage=5,.nb=4,.b={{.type=BT_SSBO,.count=1},{.binding=1,.type=BT_SSBO,.count=1},
        {.binding=2,.type=BT_SSBO,.count=1},{.binding=3,.type=BT_UBO,.count=1}}};
    pass_end();
    feedback_hold(S.feedback);
    VkDeviceSize sa=g_props.limits.minStorageBufferOffsetAlignment, ua=g_props.limits.minUniformBufferOffsetAlignment;
    VkDeviceSize align=sa>ua?sa:ua;
    VkDeviceSize stride=(sizeof(dm_vk_feedback_gather)+align-1)/align*align;
    VkDeviceSize offset=ring_alloc(4*stride,align);
    VkDescriptorBufferInfo jobs={C->ring,offset,4*stride};
    uint32_t params[11]={0};
    for (unsigned b=0;b<4;b++) if (capture->layout->stride[b]) {
        dm_vk_feedback_gather *job=(void *)(C->ringp+offset+b*stride);
        memset(job,0,sizeof *job);
        if (!dm_vk_feedback_gather_layout(capture->layout,b,job)) die("Vulkan: invalid feedback layout");
        job->source_records=capture->capacity;
        params[b]=capture->layout->stride[b];
        params[b+4]=(uint32_t)S.feedback_streams[b].size;
    }
    params[8]=capture->capacity; params[9]=capture->primitive; params[10]=(uint32_t)(stride/4);
    const VkDescriptorBufferInfo setup[]={{S.feedback->buffer,0,FEEDBACK_BYTES},jobs};
    barrier_all(C->cb);
    feedback_dispatch(&prepare,prepare_code,sizeof prepare_code,setup,params,sizeof params,1);
    barrier_all(C->cb);
    for (unsigned b=0;b<4;b++) if (capture->layout->stride[b]) {
        const p_tf_bind *dst=&S.feedback_streams[b];
        VkDeviceSize out=copy_in(dst->addr,dst->size,sa);
        const VkDescriptorBufferInfo buffers[]={capture->records,{g_zero_buf,0,4},{C->ring,out,dst->size},
            {jobs.buffer,jobs.offset+b*stride,sizeof(dm_vk_feedback_gather)}};
        feedback_dispatch(&gather,gather_code,sizeof gather_code,buffers,NULL,0,(capture->capacity+63u)/64u);
        barrier_all(C->cb);
        buffer_written(dst->addr,dst->size,C->ring,out,C->ringp+out);
    }
}
