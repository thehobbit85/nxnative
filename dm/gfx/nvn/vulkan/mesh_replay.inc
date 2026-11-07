// NVN2 uses the shared Ampere stage-memory export through ordinary compute.
// Expand triangle corners in the reader so primitive attributes stay flat even
// where several primitives share the same mesh vertices.
static shader_t *mesh_stage(int stage) {
    const prog_t *program=S.bst[stage];
    if (!program) return NULL;
    for (unsigned i=0;i<program->nstage;i++) {
        uint8_t key[8];
        if (!prog_key(program,i,key)) continue;
        shader_t *shader=shader_get(key);
        if (shader && shader->guest_stage==stage && shader->mesh.words) return shader;
    }
    return NULL;
}

static void mesh_pack(const dm_vk_meshstage *mesh, uint32_t groups,
                      VkDescriptorBufferInfo counts) {
    static const uint32_t count_code[] =
#include "mesh_pack.0.inc"
    ;
    static const uint32_t scan_code[] =
#include "mesh_pack.1.inc"
    ;
    static const uint32_t write_code[] =
#include "mesh_pack.2.inc"
    ;
    static shader_t shaders[3];
    const uint32_t *code[]={count_code,scan_code,write_code};
    const size_t bytes[]={sizeof count_code,sizeof scan_code,sizeof write_code};
    VkDescriptorBufferInfo buffers[]={G.buffers[1],G.indices,counts,G.arguments,G.buffers[2]};
    uint32_t parameters[]={groups,mesh->words,mesh->primitive,(uint32_t)(G.indices.range/4),mesh->primitives};
    for (unsigned phase=0;phase<3;phase++) {
        shader_t *shader=&shaders[phase];
        shader->stage=5;shader->nb=5;
        for (unsigned binding=0;binding<5;binding++)
            shader->b[binding]=(sbind){.binding=binding,.type=BT_SSBO,.count=1};
        feedback_dispatch(shader,code[phase],bytes[phase],buffers,parameters,sizeof parameters,
                          phase==1 ? 1 : (groups+63)/64);
        barrier_all(C->cb);
    }
}

static void task_pack(const dm_vk_meshstage *task, const dm_vk_meshstage *mesh,
                      uint32_t groups, uint32_t capacity, const VkDescriptorBufferInfo buffers[5]) {
    static const uint32_t count_code[] =
#include "task_pack.0.inc"
    ;
    static const uint32_t scan_code[] =
#include "task_pack.1.inc"
    ;
    static const uint32_t payload_code[] =
#include "task_pack.2.inc"
    ;
    static shader_t shaders[3];
    const uint32_t *code[]={count_code,scan_code,payload_code};
    const size_t bytes[]={sizeof count_code,sizeof scan_code,sizeof payload_code};
    uint32_t parameters[]={groups,task->words,task->output,(mesh->payload+3)/4,capacity};
    for (unsigned phase=0;phase<3;phase++) {
        shader_t *shader=&shaders[phase];
        shader->stage=5;shader->nb=5;
        for (unsigned binding=0;binding<5;binding++)
            shader->b[binding]=(sbind){.binding=binding,.type=BT_SSBO,.count=1};
        feedback_dispatch(shader,code[phase],bytes[phase],buffers,parameters,sizeof parameters,
                          phase==0 ? (groups+63)/64 : phase==1 ? 1 : groups);
        barrier_all(C->cb);
    }
}

static int mesh_dispatch(shader_t *shader, uint32_t groups, const VkDescriptorBufferInfo *indirect) {
    int stage=shader->guest_stage;
    shader_cpu_ready(shader,stage);
    if (!prepare_storage(&shader,NULL) || !compute_pipeline(shader)) return 0;
    g_nwb=0;
    VkDescriptorSet set=write_set(shader,stage,NULL);
    if (!set) return 0;
    vkCmdBindPipeline(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,shader->cpp);
    vkCmdBindDescriptorSets(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,shader->cpl,0,1,&set,0,NULL);
    if (indirect) vkCmdDispatchIndirect(C->cb,indirect->buffer,indirect->offset);
    else vkCmdDispatch(C->cb,groups,1,1);
    mark_image_writes(stage);barrier_all(C->cb);
    for (int i=0;i<g_nwb;i++) {
        gw_t *write=&g_wb[i];
        buffer_written(write->addr,write->size,write->buf,write->off,write->p);
    }
    global_memory_written();
    return 1;
}

static uint32_t mesh_batch(shader_t *task, shader_t *compute, shader_t *reader,
                           uint32_t first, uint32_t count, uint32_t group_base, uint32_t draw_index) {
    enum { MESH_OUTPUT,MESH_CONTROL,INDICES,DRAW_ARGUMENTS,MESH_COUNTS,
           TASK_OUTPUT,TASK_CONTROL,TASK_INPUT,TASK_COUNTS,DISPATCH_ARGUMENTS,RANGES };
    const dm_vk_meshstage *mesh=&compute->mesh;
    VkDeviceSize budget=256u<<20;
    if (getenv("DM_VK_TEST_MESH_LIMIT")) budget=task ? 8u<<20 : 65536;
    VkDeviceSize alignment=g_props.limits.minStorageBufferOffsetAlignment;
    if (alignment<g_props.limits.minUniformBufferOffsetAlignment) alignment=g_props.limits.minUniformBufferOffsetAlignment;
    uint32_t fanout=task ? 1024 : 1;
    uint32_t limit=g_props.limits.maxComputeWorkGroupCount[0]/fanout;
    uint32_t groups=count<limit ? count : limit,capacity=0;
    unsigned range_count=task ? RANGES : TASK_OUTPUT;
    VkDeviceSize sizes[RANGES],offsets[RANGES],total=0;
    while (groups) {
        capacity=groups*fanout;
        sizes[MESH_OUTPUT]=(uint64_t)capacity*mesh->words*4;
        sizes[MESH_CONTROL]=32;
        sizes[INDICES]=(uint64_t)capacity*mesh->primitives*3*4;
        sizes[DRAW_ARGUMENTS]=sizeof(VkDrawIndexedIndirectCommand);
        sizes[MESH_COUNTS]=((uint64_t)capacity+(capacity+63u)/64)*4;
        if (task) {
            sizes[TASK_OUTPUT]=(uint64_t)groups*task->mesh.words*4;
            sizes[TASK_CONTROL]=32;
            sizes[TASK_INPUT]=(uint64_t)capacity*((mesh->payload+3)/4+1)*4;
            sizes[TASK_COUNTS]=((uint64_t)groups+(groups+63u)/64)*4;
            sizes[DISPATCH_ARGUMENTS]=sizeof(VkDispatchIndirectCommand);
        }
        total=0;
        int fits=1;
        for (unsigned i=0;i<range_count;i++) {
            total=(total+alignment-1)/alignment*alignment;
            offsets[i]=total;
            if (total>budget || sizes[i]>g_props.limits.maxStorageBufferRange || sizes[i]>budget-total) { fits=0;break; }
            total+=sizes[i];
        }
        if (fits) break;
        groups/=2;
    }
    if (!groups) { RED_ONCE(125,"mesh output exceeds device scratch limits");return 0; }
    pass_end();
    if (atomic_load(&g_extra_ring_bytes)>budget-total) {
        mtl_wait(mtl_sub_commit(),UINT64_MAX);begin_segment();
    }
    VkDeviceMemory memory;
    uint8_t *mapped;
    VkBuffer scratch=buf_new(total,ALL_BUF_USAGE,&memory,(void **)&mapped);
    atomic_fetch_add(&g_extra_ring_bytes,total);
    VkDescriptorBufferInfo ranges[RANGES];
    for (unsigned i=0;i<range_count;i++) ranges[i]=(VkDescriptorBufferInfo){scratch,offsets[i],sizes[i]};
    uint32_t control[]={first,capacity,0,capacity,0,capacity,(mesh->payload+3)/4+1,group_base};
    memcpy(mapped+offsets[MESH_CONTROL],control,sizeof control);
    G.phase=3;
    G.buffers[0]=(VkDescriptorBufferInfo){g_zero_buf,0,4};
    G.indices=ranges[INDICES];G.arguments=ranges[DRAW_ARGUMENTS];
    uint32_t completed=0;
    if (task) {
        uint32_t task_control[]={first,groups,0,groups,0,0,0,group_base};
        memcpy(mapped+offsets[TASK_CONTROL],task_control,sizeof task_control);
        G.buffers[1]=ranges[TASK_OUTPUT];G.buffers[2]=ranges[TASK_CONTROL];
        if (!mesh_dispatch(task,groups,NULL)) goto retire;
        VkDescriptorBufferInfo pack[]={ranges[TASK_OUTPUT],ranges[TASK_INPUT],ranges[TASK_COUNTS],
                                      ranges[DISPATCH_ARGUMENTS],ranges[MESH_CONTROL]};
        task_pack(&task->mesh,mesh,groups,capacity,pack);
        G.buffers[0]=ranges[TASK_INPUT];
    }
    G.buffers[1]=ranges[MESH_OUTPUT];G.buffers[2]=ranges[MESH_CONTROL];
    if (!mesh_dispatch(compute,groups,task ? &ranges[DISPATCH_ARGUMENTS] : NULL)) goto retire;
    if (!S.rasterizer_discard) {
        mesh_pack(mesh,capacity,ranges[MESH_COUNTS]);
        G.phase=2;G.vertex=reader;
        p_prim_restart restart=S.restart;S.restart.on=0;
        p_draw raster={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=1,.instances=1};
        draw_direct(&raster,NULL,draw_index);
        S.restart=restart;pass_end();
    }
    completed=groups;
retire:
    memset(&G,0,sizeof G);
    void *extra=realloc(C->extra,(C->nextra+1)*sizeof *C->extra);
    if (!extra) die("Vulkan: mesh buffer retirement allocation");
    C->extra=extra;
    C->extra[C->nextra++]=(typeof(*C->extra)){.b=scratch,.m=memory,.size=total};
    return completed;
}

static void mesh_draw(const p_draw *draw) {
    if (vk_skip()&SK_DRAW) return;
    shader_t *mesh=mesh_stage(4),*task=mesh_stage(3);
    if (!mesh || !mesh->mod || !!task!=!!mesh->mesh.task ||
        (task && (!task->mod || task->mesh.payload<mesh->mesh.payload))) {
        RED_ONCE(125,"mesh draw needs compatible task and mesh stages");return;
    }
    shader_t *stages[]={task,mesh};
    for (unsigned i=0;i<2;i++) if (stages[i]) {
        const dm_vk_meshstage *stage=&stages[i]->mesh;
        if (stage->threads>g_props.limits.maxComputeWorkGroupInvocations ||
            stage->threads>g_props.limits.maxComputeWorkGroupSize[0] || stage->words*4>g_props.limits.maxComputeSharedMemorySize) {
            RED_ONCE(125,"task/mesh stage exceeds device workgroup limits");return;
        }
    }
    char key[32];snprintf(key,sizeof key,"%.16s.mr",mesh->key);
    shader_t *reader=shader_by_key(key);
    if (!reader || !reader->mod) { RED_ONCE(125,"mesh draw needs its exported raster reader");return; }
    uint32_t arguments[]={(uint32_t)draw->count,(uint32_t)draw->first};
    if (draw->ind && !read_draw_args(draw->ind,arguments,sizeof arguments)) return;
    if (!draw->ind && draw->count<=0) return;
    uint32_t count=arguments[0],first=arguments[1];
    if ((uint64_t)first+count>UINT64_C(0x100000000)) { RED_ONCE(125,"mesh group IDs exceed 32 bits");return; }
    uint32_t group_base=0;
    while (count) {
        uint32_t completed=mesh_batch(task,mesh,reader,first,count,group_base,(uint32_t)draw->mdi_i);
        if (!completed) return;
        first+=completed;count-=completed;group_base+=completed;
    }
}
