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
    VkDescriptorBufferInfo buffers[]={G.buffers[1],G.indices,counts,G.arguments};
    uint32_t parameters[]={groups,mesh->words,mesh->primitive,(uint32_t)(G.indices.range/4),mesh->primitives};
    for (unsigned phase=0;phase<3;phase++) {
        shader_t *shader=&shaders[phase];
        shader->stage=5;shader->nb=4;
        for (unsigned binding=0;binding<4;binding++)
            shader->b[binding]=(sbind){.binding=binding,.type=BT_SSBO,.count=1};
        feedback_dispatch(shader,code[phase],bytes[phase],buffers,parameters,sizeof parameters,
                          phase==1 ? 1 : (groups+63)/64);
        barrier_all(C->cb);
    }
}

static uint32_t mesh_batch(shader_t *compute, shader_t *reader, uint32_t first,
                           uint32_t count, uint32_t draw_index) {
    const dm_vk_meshstage *mesh=&compute->mesh;
    const VkDeviceSize budget=getenv("DM_VK_TEST_MESH_LIMIT") ? 65536 : 256u<<20;
    VkDeviceSize alignment=g_props.limits.minStorageBufferOffsetAlignment;
    if (alignment<g_props.limits.minUniformBufferOffsetAlignment) alignment=g_props.limits.minUniformBufferOffsetAlignment;
    uint32_t groups=count<g_props.limits.maxComputeWorkGroupCount[0] ? count : g_props.limits.maxComputeWorkGroupCount[0];
    VkDeviceSize sizes[5],offsets[5],total=0;
    while (groups) {
        sizes[0]=(uint64_t)groups*mesh->words*4;
        sizes[1]=32;
        sizes[2]=(uint64_t)groups*mesh->primitives*3*4;
        sizes[3]=sizeof(VkDrawIndexedIndirectCommand);
        sizes[4]=(groups+(groups+63)/64)*4u;
        total=0;
        int fits=1;
        for (unsigned i=0;i<5;i++) {
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
    VkDescriptorBufferInfo ranges[5];
    for (unsigned i=0;i<5;i++) ranges[i]=(VkDescriptorBufferInfo){scratch,offsets[i],sizes[i]};
    uint32_t control[]={first,groups,0,groups,0,0,0,0};
    memcpy(mapped+offsets[1],control,sizeof control);
    G.phase=3;
    G.buffers[0]=(VkDescriptorBufferInfo){g_zero_buf,0,4};
    G.buffers[1]=ranges[0];G.buffers[2]=ranges[1];
    G.indices=ranges[2];G.arguments=ranges[3];
    shader_cpu_ready(compute,4);
    uint32_t completed=0;
    if (prepare_storage(&compute,NULL) && compute_pipeline(compute)) {
        g_nwb=0;
        VkDescriptorSet set=write_set(compute,4,NULL);
        if (set) {
            vkCmdBindPipeline(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,compute->cpp);
            vkCmdBindDescriptorSets(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,compute->cpl,0,1,&set,0,NULL);
            vkCmdDispatch(C->cb,groups,1,1);
            mark_image_writes(4);barrier_all(C->cb);
            for (int i=0;i<g_nwb;i++) {
                gw_t *write=&g_wb[i];
                buffer_written(write->addr,write->size,write->buf,write->off,write->p);
            }
            global_memory_written();
            if (!S.rasterizer_discard) {
                mesh_pack(mesh,groups,ranges[4]);
                G.phase=2;G.vertex=reader;
                p_prim_restart restart=S.restart;S.restart.on=0;
                p_draw raster={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=1,.instances=1};
                draw_direct(&raster,NULL,draw_index);
                S.restart=restart;pass_end();
            }
            completed=groups;
        }
    }
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
    if (!mesh || !mesh->mod || mesh->mesh.task || task) {
        RED_ONCE(125,"mesh draw needs a standalone mesh stage; task launch replay remains unavailable");return;
    }
    if (mesh->mesh.threads>g_props.limits.maxComputeWorkGroupInvocations ||
        mesh->mesh.threads>g_props.limits.maxComputeWorkGroupSize[0] || mesh->mesh.words*4>g_props.limits.maxComputeSharedMemorySize) {
        RED_ONCE(125,"mesh stage exceeds device workgroup limits");return;
    }
    char key[32];snprintf(key,sizeof key,"%.16s.mr",mesh->key);
    shader_t *reader=shader_by_key(key);
    if (!reader || !reader->mod) { RED_ONCE(125,"mesh draw needs its exported raster reader");return; }
    uint32_t arguments[]={(uint32_t)draw->count,(uint32_t)draw->first};
    if (draw->ind && !read_draw_args(draw->ind,arguments,sizeof arguments)) return;
    if (!draw->ind && draw->count<=0) return;
    uint32_t count=arguments[0],first=arguments[1];
    if ((uint64_t)first+count>UINT64_C(0x100000000)) { RED_ONCE(125,"mesh group IDs exceed 32 bits");return; }
    while (count) {
        uint32_t completed=mesh_batch(mesh,reader,first,count,(uint32_t)draw->mdi_i);
        if (!completed) return;
        first+=completed;count-=completed;
    }
}
