// Reuse the shared Maxwell geometry-as-compute ABI and primitive assembly.
// Capture uses native vertex fetch, then an ordered GPU compaction feeds the
// exported reader through an indexed draw. Guest stage 2 retains its bindings.
static int geometry_index_order(const void *a, const void *b) {
    uint32_t x=*(const uint32_t *)a, y=*(const uint32_t *)b;
    return (x>y)-(x<y);
}

static void geometry_pack(const dm_vk_geometry *geometry, uint32_t groups,
                          VkDescriptorBufferInfo counts) {
    static const uint32_t count_code[] =
#include "geometry_pack.0.inc"
    ;
    static const uint32_t scan_code[] =
#include "geometry_pack.1.inc"
    ;
    static const uint32_t write_code[] =
#include "geometry_pack.2.inc"
    ;
    static shader_t shaders[3];
    const uint32_t *code[]={count_code,scan_code,write_code};
    const size_t bytes[]={sizeof count_code,sizeof scan_code,sizeof write_code};
    VkDescriptorBufferInfo buffers[]={G.buffers[2],G.indices,counts,G.arguments};
    uint32_t parameters[]={groups,geometry->index_stride,geometry->output,(uint32_t)(G.indices.range/4)};
    for (unsigned phase=0;phase<3;phase++) {
        shader_t *shader=&shaders[phase];
        shader->stage=5; shader->nb=4;
        for (unsigned binding=0;binding<4;binding++)
            shader->b[binding]=(sbind){.binding=binding,.type=BT_SSBO,.count=1};
        feedback_dispatch(shader,code[phase],bytes[phase],buffers,parameters,sizeof parameters,
                          phase==1 ? 1 : (groups+63)/64);
        barrier_all(C->cb);
    }
}

static void geometry_draw(shader_t *vertex, const p_draw *draw, const p_draw_idx *indexed, uint32_t draw_index) {
    if (!vertex || !vertex->mod || !g_vertex_stores || S.bst[3] || S.bst[4]) {
        RED_ONCE(122,"geometry capture needs a vertex shader with storage writes and no tessellation");
        return;
    }
    if (S.feedback_active) {
        RED_ONCE(123,"geometry transform-feedback packing remains unavailable");
        return;
    }
    int primitive=draw ? draw->prim : indexed->prim;
    int count=draw ? draw->count : indexed->count;
    int instances=draw ? draw->instances : indexed->instances;
    int topology=nvn_geo_topology(primitive);
    uint32_t first_instance=(uint32_t)(draw ? draw->base_instance : indexed->base_instance);
    if (count<=0 || instances<0) return;
    if (!instances) instances=1;
    if (topology<0 || count>(1<<22) || (indexed && (indexed->itype<0 || indexed->itype>2)) ||
        (uint64_t)first_instance+instances>UINT64_C(0x100000000)) {
        RED_ONCE(122,"geometry primitive or vertex/instance range exceeds replay limits");
        return;
    }
    shader_t *base=NULL;
    for (unsigned stage=0;stage<S.bst[2]->nstage;stage++) {
        uint8_t key[8];
        if (!prog_key(S.bst[2],stage,key)) continue;
        shader_t *candidate=shader_get(key);
        if (candidate && candidate->stage==2) { base=candidate; break; }
    }
    if (!base) { RED_ONCE(122,"bound geometry program has no geometry stage"); return; }
    char name[32];
    snprintf(name,sizeof name,"%.16s.gc%d",base->key,topology);
    shader_t *compute=shader_by_key(name);
    snprintf(name,sizeof name,"%.16s.gr%d",base->key,topology);
    shader_t *reader=shader_by_key(name);
    shader_t *capture=feedback_capture_shader(vertex);
    if (!compute || !compute->mod || !reader || !reader->mod || !capture || !capture->mod ||
        !compute->geometry.stride || compute->geometry.topology!=(uint32_t)topology ||
        compute->geometry.reader || !reader->geometry.reader || reader->geometry.topology!=(uint32_t)topology ||
        compute->geometry.stride!=reader->geometry.stride || compute->geometry.vertices!=reader->geometry.vertices ||
        compute->geometry.invocations!=reader->geometry.invocations || compute->geometry.output!=reader->geometry.output) {
        RED_ONCE(122,"geometry draw needs its exported capture, compute and reader helpers");
        return;
    }
    dm_vk_geometry geometry=compute->geometry;
    shader_cpu_ready(vertex,0);
    shader_cpu_ready(compute,2);
    unsigned index_bytes=indexed ? 1u<<indexed->itype : 0;
    if (indexed) cpu_ready(indexed->ibuf,(uint64_t)count*index_bytes);
    uint32_t *order=malloc((size_t)count*sizeof *order);
    if (!order) die("Vulkan: geometry index allocation");
    int restart=indexed && S.restart.on;
    uint32_t restart_index=S.restart.index & (index_bytes==1 ? UINT8_MAX : index_bytes==2 ? UINT16_MAX : UINT32_MAX);
    uint32_t lo=UINT32_MAX, hi=0;
    for (int i=0;i<count;i++) {
        uint32_t value=(uint32_t)i;
        if (indexed) { value=0; memcpy(&value,(const uint8_t *)(uintptr_t)indexed->ibuf+(size_t)i*index_bytes,index_bytes); }
        order[i]=value;
        if (restart && value==restart_index) continue;
        if (value<lo) lo=value;
        if (value>hi) hi=value;
    }
    size_t map_words=nvn_geo_assemble(primitive,order,count,restart,restart_index,NULL,0);
    if (lo==UINT32_MAX || map_words==SIZE_MAX || !map_words) { free(order); return; }
    uint64_t span=(uint64_t)hi-lo+1;
    int64_t first=indexed ? (int64_t)lo+indexed->base_vertex : draw->first;
    uint64_t primitives=map_words/nvn_geo_width(topology);
    uint64_t groups=primitives*(uint64_t)instances*geometry.invocations;
    uint64_t records=span*(uint64_t)instances;
    if (groups>UINT32_MAX || records>g_props.limits.maxStorageBufferRange/(GEO_CAPTURE_WORDS*4)) {
        free(order);RED_ONCE(122,"geometry capture exceeds the device's storage-buffer range");return;
    }
    uint64_t emitted=groups*geometry.vertices;
    uint64_t output_width=geometry.output==1 ? 1 : geometry.output==6 ? 2 : 3;
    VkDeviceSize sizes[]={records*GEO_CAPTURE_WORDS*4,emitted*geometry.stride*4,
        groups*geometry.index_stride*4,32,map_words*4,emitted*output_width*4,
        sizeof(VkDrawIndexedIndirectCommand),(groups+(groups+63)/64)*4,24};
    VkDeviceSize alignment=g_props.limits.minStorageBufferOffsetAlignment;
    if (alignment<g_props.limits.minUniformBufferOffsetAlignment) alignment=g_props.limits.minUniformBufferOffsetAlignment;
    if (alignment<g_props.limits.minTexelBufferOffsetAlignment) alignment=g_props.limits.minTexelBufferOffsetAlignment;
    VkDeviceSize total=0;
    int bounded=first>=0 && (uint64_t)first+span<=UINT64_C(0x100000000) && records<=UINT32_MAX && groups<=UINT32_MAX &&
        (primitives+31)/32<=g_props.limits.maxComputeWorkGroupCount[0] &&
        (uint32_t)instances<=g_props.limits.maxComputeWorkGroupCount[1] && geometry.invocations<=g_props.limits.maxComputeWorkGroupCount[2] &&
        (groups+63)/64<=g_props.limits.maxComputeWorkGroupCount[0] && map_words<=g_props.limits.maxTexelBufferElements;
    for (unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
        total=(total+alignment-1)/alignment*alignment;
        if (total>(256u<<20) || sizes[i]>g_props.limits.maxStorageBufferRange || sizes[i]>(256u<<20)-total) { bounded=0; break; }
        total+=sizes[i];
    }
    if (!bounded) { free(order); RED_ONCE(122,"geometry draw exceeds the current scratch/dispatch bounds"); return; }

    // This buffer survives any guest/host synchronization inside the three
    // stages. Retire it with the last submission that references the draw.
    VkDeviceMemory memory;
    uint8_t *mapped;
    VkBuffer scratch=buf_new(total,ALL_BUF_USAGE,&memory,(void **)&mapped);
    atomic_fetch_add(&g_extra_ring_bytes,total);
    VkDescriptorBufferInfo ranges[sizeof sizes/sizeof *sizes];
    VkDeviceSize offset=0;
    for (unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
        offset=(offset+alignment-1)/alignment*alignment;
        ranges[i]=(VkDescriptorBufferInfo){scratch,offset,sizes[i]};offset+=sizes[i];
    }
    G.phase=1;G.vertex=capture;
    memcpy(G.buffers,ranges,sizeof G.buffers);G.indices=ranges[5];G.arguments=ranges[6];
    G.capture=(capture_draw){.records=ranges[0],.control=ranges[8]};
    uint32_t *map=(void *)(mapped+ranges[4].offset);
    nvn_geo_assemble(primitive,order,count,restart,restart_index,map,map_words);
    for (size_t i=0;i<map_words;i++) map[i]-=lo;
    uint32_t info[]={span,(uint32_t)instances,0,0,(uint32_t)primitives,0,0,0};
    memcpy(mapped+ranges[3].offset,info,sizeof info);
    uint32_t control[]={(uint32_t)first,(uint32_t)span,first_instance,(uint32_t)instances,0,(uint32_t)records};
    memcpy(mapped+ranges[8].offset,control,sizeof control);
    int discard=S.rasterizer_discard;
    p_prim_restart saved_restart=S.restart;
    S.rasterizer_discard=1;S.restart.on=0;
    uint64_t draws_before=g_stat_draws;
    if (indexed) {
        qsort(order,count,sizeof *order,geometry_index_order);
        unsigned unique=0;
        for (int i=0;i<count;i++) if ((!restart || order[i]!=restart_index) && (!unique || order[i]!=order[unique-1])) order[unique++]=order[i];
        p_draw_idx points={.prim=NVN_DRAW_PRIMITIVE_POINTS,.count=unique,.itype=2,.ibuf=(uintptr_t)order,
            .base_vertex=indexed->base_vertex,.instances=instances,.base_instance=first_instance};
        draw_direct(NULL,&points,draw_index);
    } else {
        p_draw points={.prim=NVN_DRAW_PRIMITIVE_POINTS,.first=draw->first,.count=count,.instances=instances,.base_instance=first_instance};
        draw_direct(&points,NULL,draw_index);
    }
    free(order);
    pass_end();barrier_all(C->cb);
    S.rasterizer_discard=discard;
    if (g_stat_draws==draws_before) goto retire;
    G.phase=3;
    vkCmdFillBuffer(C->cb,ranges[2].buffer,ranges[2].offset,ranges[2].range,UINT32_MAX);
    barrier_all(C->cb);
    shader_cpu_ready(compute,2);
    if (prepare_storage(&compute,NULL) && compute_pipeline(compute)) {
        g_nwb=0;
        g_draw_base_instance=first_instance;
        VkDescriptorSet set=write_set(compute,2,NULL);
        if (set) {
            vkCmdBindPipeline(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,compute->cpp);
            vkCmdBindDescriptorSets(C->cb,VK_PIPELINE_BIND_POINT_COMPUTE,compute->cpl,0,1,&set,0,NULL);
            vkCmdDispatch(C->cb,(uint32_t)((primitives+31)/32),instances,geometry.invocations);
            mark_image_writes(2);barrier_all(C->cb);
            for (int i=0;i<g_nwb;i++) {
                gw_t *write=&g_wb[i];
                buffer_written(write->addr,write->size,write->buf,write->off,write->p);
            }
            geometry_pack(&geometry,(uint32_t)groups,ranges[7]);
            if (!discard) {
                G.phase=2;G.vertex=reader;
                p_draw raster={.prim=geometry.output==1 ? NVN_DRAW_PRIMITIVE_POINTS : geometry.output==6 ? NVN_DRAW_PRIMITIVE_LINES : NVN_DRAW_PRIMITIVE_TRIANGLES,
                    .count=1,.instances=1,.base_instance=first_instance};
                draw_direct(&raster,NULL,draw_index);
                pass_end();
            }
        }
    }
retire:
    S.restart=saved_restart;
    memset(&G,0,sizeof G);
    void *extra=realloc(C->extra,(C->nextra+1)*sizeof *C->extra);
    if (!extra) die("Vulkan: geometry buffer retirement allocation");
    C->extra=extra;
    C->extra[C->nextra++]=(typeof(*C->extra)){.b=scratch,.m=memory,.size=total};
}
