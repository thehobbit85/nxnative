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

// Match the reference's list splitting. Each child keeps the guest draw ID,
// first instance and primitive base while sizing its own capture/output data.
static int geometry_split(shader_t *vertex, const p_draw *draw, const p_draw_idx *indexed,
                          uint32_t draw_index, uint32_t primitive_base) {
    int primitive=draw ? draw->prim : indexed->prim;
    int width=primitive==0 ? 1 : primitive==1 ? 2 : primitive==4 ? 3 : primitive==10 ? 4 : primitive==12 ? 6 : 0;
    if (!width || (indexed && S.restart.on)) return 0;
    int count=draw ? draw->count : indexed->count;
    int instances=draw ? draw->instances : indexed->instances;
    if (count<width) return 0;
    p_draw left=draw ? *draw : (p_draw){0}, right=left;
    p_draw_idx indexed_left=indexed ? *indexed : (p_draw_idx){0}, indexed_right=indexed_left;
    uint32_t right_base=primitive_base;
    if (instances>1) {
        int first=instances/2;
        if (draw) {
            left.instances=first;right.instances=instances-first;
            right.base_instance=(uint32_t)draw->base_instance+(uint32_t)first;
        } else {
            indexed_left.instances=first;indexed_right.instances=instances-first;
            indexed_right.base_instance=(uint32_t)indexed->base_instance+(uint32_t)first;
        }
    } else {
        int primitives=count/width;
        if (primitives<2) return 0;
        int first=primitives/2*width;
        right_base+=(uint32_t)(first/width);
        if (draw) {
            left.count=first;right.count=count-first;
            right.first=(uint32_t)draw->first+(uint32_t)first;
        } else {
            indexed_left.count=first;indexed_right.count=count-first;
            indexed_right.ibuf+=(uint64_t)first<<indexed->itype;
        }
    }
    geometry_draw(vertex,draw ? &left : NULL,indexed ? &indexed_left : NULL,draw_index,primitive_base);
    geometry_draw(vertex,draw ? &right : NULL,indexed ? &indexed_right : NULL,draw_index,right_base);
    return 1;
}

static void geometry_draw(shader_t *vertex, const p_draw *draw, const p_draw_idx *indexed,
                          uint32_t draw_index, uint32_t primitive_base) {
    if (!vertex || !vertex->mod || !g_vertex_stores || S.bst[3] || S.bst[4]) {
        RED_ONCE(122,"geometry capture needs a vertex shader with storage writes and no tessellation");
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
    uint32_t *topology_map=malloc(map_words*sizeof *topology_map);
    if (!topology_map) die("Vulkan: geometry topology allocation");
    nvn_geo_assemble(primitive,order,count,restart,restart_index,topology_map,map_words);
    unsigned unique=0;
    if (indexed) {
        qsort(order,count,sizeof *order,geometry_index_order);
        for (int i=0;i<count;i++) if ((!restart || order[i]!=restart_index) && (!unique || order[i]!=order[unique-1])) order[unique++]=order[i];
    }
    uint64_t span=(uint64_t)hi-lo+1;
    size_t runs=indexed ? nvn_geo_capture_runs(order,unique) : 1;
    // The shared Metal path captures sparse IDs in contiguous runs. Preserve
    // their real vertex IDs, while the GS map names compact record positions.
    const uint64_t compact_threshold=((1u<<20)+GEO_CAPTURE_WORDS*4-1)/(GEO_CAPTURE_WORDS*4);
    int compact=indexed && span>unique && (span-unique)*(uint64_t)instances>=compact_threshold && runs<=64;
    if (!compact) runs=1;
    uint64_t capture_span=compact ? unique : span;
    if (compact) {
        if (!nvn_geo_compact_map(topology_map,map_words,0,order,unique,topology_map))
            die("Vulkan: geometry topology refers to an uncaptured vertex");
    } else for (size_t i=0;i<map_words;i++) topology_map[i]-=lo;
    int64_t first=indexed ? (int64_t)lo+indexed->base_vertex : draw->first;
    if (first<0 || (uint64_t)first+span>UINT64_C(0x100000000)) {
        free(topology_map);free(order);
        RED_ONCE(122,"geometry vertex range is outside 32-bit vertex IDs");return;
    }
    uint64_t primitives=map_words/nvn_geo_width(topology);
    uint64_t groups=primitives*(uint64_t)instances*geometry.invocations;
    uint64_t records=capture_span*(uint64_t)instances;
    if (groups>UINT32_MAX || records>g_props.limits.maxStorageBufferRange/(GEO_CAPTURE_WORDS*4)) {
        free(topology_map);free(order);
        if (!geometry_split(vertex,draw,indexed,draw_index,primitive_base))
            RED_ONCE(122,"geometry capture exceeds the device's storage-buffer range");
        return;
    }
    uint64_t emitted=groups*geometry.vertices;
    uint64_t output_width=geometry.output==1 ? 1 : geometry.output==6 ? 2 : 3;
    uint32_t feedback_count=S.feedback_active && reader->feedback_layout ?
        feedback_capacity(reader->feedback_layout,emitted*output_width) : 0;
    shader_t *feedback_entry=NULL;
    if (feedback_count) {
        feedback_entry=feedback_capture_shader(reader);
        if (!feedback_entry || !feedback_entry->mod || !feedback_entry->capture_entry ||
            !feedback_entry->feedback_layout ||
            memcmp(feedback_entry->feedback_layout,reader->feedback_layout,sizeof *reader->feedback_layout)) {
            free(topology_map);free(order);RED_ONCE(123,"geometry feedback needs its exported reader capture entry");return;
        }
    }
    VkDeviceSize uniform_alignment=g_props.limits.minUniformBufferOffsetAlignment;
    VkDeviceSize control_stride=(24+uniform_alignment-1)/uniform_alignment*uniform_alignment;
    VkDeviceSize sizes[]={records*GEO_CAPTURE_WORDS*4,emitted*geometry.stride*4,
        groups*geometry.index_stride*4,32,map_words*4,emitted*output_width*4,
        sizeof(VkDrawIndexedIndirectCommand),(groups+(groups+63)/64)*4,control_stride*runs,
        feedback_count ? emitted*GEO_CAPTURE_WORDS*4 : 0,feedback_count ? 24 : 0};
    VkDeviceSize alignment=g_props.limits.minStorageBufferOffsetAlignment;
    if (alignment<g_props.limits.minUniformBufferOffsetAlignment) alignment=g_props.limits.minUniformBufferOffsetAlignment;
    if (alignment<g_props.limits.minTexelBufferOffsetAlignment) alignment=g_props.limits.minTexelBufferOffsetAlignment;
    VkDeviceSize total=0;
    VkDeviceSize scratch_limit=getenv("DM_VK_TEST_GEOMETRY_LIMIT") ? 65536 : 256u<<20;
    int bounded=records<=UINT32_MAX && groups<=UINT32_MAX &&
        (primitives+31)/32<=g_props.limits.maxComputeWorkGroupCount[0] &&
        (uint32_t)instances<=g_props.limits.maxComputeWorkGroupCount[1] && geometry.invocations<=g_props.limits.maxComputeWorkGroupCount[2] &&
        (groups+63)/64<=g_props.limits.maxComputeWorkGroupCount[0] && map_words<=g_props.limits.maxTexelBufferElements;
    for (unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
        total=(total+alignment-1)/alignment*alignment;
        if (total>scratch_limit || sizes[i]>g_props.limits.maxStorageBufferRange || sizes[i]>scratch_limit-total) { bounded=0; break; }
        total+=sizes[i];
    }
    if (!bounded) {
        free(topology_map);free(order);
        if (!geometry_split(vertex,draw,indexed,draw_index,primitive_base))
            RED_ONCE(122,"geometry draw exceeds the current scratch/dispatch bounds");
        return;
    }

    // This buffer survives any guest/host synchronization inside the three
    // stages. Retire it with the last submission that references the draw.
    // Complete prior children before their retained scratch exceeds the budget.
    if (atomic_load(&g_extra_ring_bytes)>scratch_limit-total) {
        mtl_wait(mtl_sub_commit(),UINT64_MAX);begin_segment();
    }
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
    G.capture.control.range=24;
    memcpy(mapped+ranges[4].offset,topology_map,map_words*4);free(topology_map);
    uint32_t info[]={capture_span,(uint32_t)instances,0,0,(uint32_t)primitives,primitive_base,0,0};
    memcpy(mapped+ranges[3].offset,info,sizeof info);
    uint32_t control[]={(uint32_t)first,(uint32_t)capture_span,first_instance,(uint32_t)instances,0,(uint32_t)records};
    unsigned begin=0;
    for (size_t run=0;run<runs;run++) {
        if (compact) control[0]=(uint32_t)(first+order[begin]-lo-begin);
        memcpy(mapped+ranges[8].offset+run*control_stride,control,sizeof control);
        if (compact) {
            unsigned end=begin+1;
            while (end<unique && order[end]==order[end-1]+1) end++;
            begin=end;
        }
    }
    int discard=S.rasterizer_discard;
    p_prim_restart saved_restart=S.restart;
    S.rasterizer_discard=1;S.restart.on=0;
    uint64_t draws_before=g_stat_draws;
    if (indexed) {
        begin=0;
        for (size_t run=0;run<runs;run++) {
            unsigned end=unique;
            if (compact) {
                end=begin+1;
                while (end<unique && order[end]==order[end-1]+1) end++;
            }
            G.capture.control.offset=ranges[8].offset+run*control_stride;
            p_draw_idx points={.prim=NVN_DRAW_PRIMITIVE_POINTS,.count=end-begin,.itype=2,.ibuf=(uintptr_t)(order+begin),
                .base_vertex=indexed->base_vertex,.instances=instances,.base_instance=first_instance};
            draw_direct(NULL,&points,draw_index);begin=end;
        }
    } else {
        p_draw points={.prim=NVN_DRAW_PRIMITIVE_POINTS,.first=draw->first,.count=count,.instances=instances,.base_instance=first_instance};
        draw_direct(&points,NULL,draw_index);
    }
    free(order);
    pass_end();barrier_all(C->cb);
    S.rasterizer_discard=discard;
    if (g_stat_draws-draws_before!=runs) goto retire;
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
            global_memory_written();
            geometry_pack(&geometry,(uint32_t)groups,ranges[7]);
            if (feedback_count) {
                // Capture each reader vertex once. Strip decomposition can
                // repeat an output index, so the later gather expands those
                // references without racing writes to the same record.
                uint32_t reader_control[]={0,(uint32_t)emitted,0,1,0,(uint32_t)emitted};
                memcpy(mapped+ranges[10].offset,reader_control,sizeof reader_control);
                G.phase=1;G.vertex=feedback_entry;
                G.capture=(capture_draw){.records=ranges[9],.control=ranges[10]};
                S.rasterizer_discard=1;
                p_draw points={.prim=NVN_DRAW_PRIMITIVE_POINTS,.count=(int)emitted,.instances=1};
                uint64_t before=g_stat_draws;
                draw_direct(&points,NULL,draw_index);
                S.rasterizer_discard=discard;
                if (g_stat_draws!=before) {
                    capture_draw packed={.records=ranges[9],.order=G.indices,.indirect=G.arguments,
                        .layout=reader->feedback_layout,.source_records=(uint32_t)emitted,
                        .capacity=feedback_count,.primitive=(uint32_t)output_width};
                    feedback_finish(&packed);
                }
            }
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
