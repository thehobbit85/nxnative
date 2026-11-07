// BC sampling on Android already uses the CPU decoder. Resolve raw encoders
// through the same path after an affected command so later commands see both
// the original blocks and their decoded image. The GPU wait is a known cost
// of this fallback; a GPU decoder can remove it without changing the ABI.
static void block_targets_reload(void) {
    for (int i=0;i<P.n;i++) {
        const block_target *target=&P.blocks[i];
        if (!target->base || !P.cv[i]) continue;
        // A copy, another queue or a CPU update can replace the encoded
        // blocks while this attachment remains bound. Reload at command use.
        pass_end();
        cpu_ready((uintptr_t)target->texture.mem,target->texture.L.size);
        VkDeviceSize offset=ring_alloc(target->bytes,16);
        if (!storage_texels(&target->texture,target->region.imageSubresource.mipLevel,C->ringp+offset,
                            target->extent.bytes,target->bytes,target->extent,0))
            die("Vulkan: compressed attachment storage changed before drawing");
        VkBufferImageCopy copy={offset,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{P.ct[i]->w,P.ct[i]->h,1}};
        barrier_all(C->cb);
        vkCmdCopyBufferToImage(C->cb,C->ring,P.ct[i]->img,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        barrier_all(C->cb);
    }
}
static void block_targets_finish(void) {
    uint8_t *raw[8]={0};int any=0;
    for (int i=0;i<P.n;i++) if (P.blocks[i].base && P.cv[i]) { any=1;break; }
    if (!any) return;
    pass_end();
    for (int i=0;i<P.n;i++) {
        const block_target *target=&P.blocks[i];
        if (!target->base || !P.cv[i]) continue;
        raw[i]=malloc(target->bytes);
        if (!raw[i]) die("Vulkan: compressed attachment readback allocation");
        atomic_fetch_add(&target->base->present_refs,1);
        atomic_fetch_add(&P.ct[i]->present_refs,1);
        VkDeviceSize offset=ring_alloc(target->bytes,16);
        VkBufferImageCopy copy={offset,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{P.ct[i]->w,P.ct[i]->h,1}};
        vkCmdCopyImageToBuffer(C->cb,P.ct[i]->img,VK_IMAGE_LAYOUT_GENERAL,C->ring,1,&copy);
        post_add((post_t){.kind=3,.dst=raw[i],.src=C->ringp+offset,.n=target->bytes});
    }
    mtl_wait(mtl_sub_commit(),UINT64_MAX);begin_segment();
    for (int i=0;i<P.n;i++) if (raw[i]) {
        block_target *target=&P.blocks[i];
        VkDeviceSize offset=ring_put(raw[i],target->bytes,16);free(raw[i]);
        if (!write_stored_image(&target->texture,target->base,target->region,target->extent,offset))
            die("Vulkan: compressed attachment storage changed during resolve");
        target->base->gpu=0;target->base->uploaded=1;target->base->dirty=0;
        // A finalized image can still be held by the command being replayed.
        // Retire it after this segment's decoded upload has completed.
        pthread_mutex_lock(&g_gbmu);
        if (target->base->dead && target->base->dead<g_fence+1) target->base->dead=g_fence+1;
        if (P.ct[i]->dead && P.ct[i]->dead<g_fence+1) P.ct[i]->dead=g_fence+1;
        atomic_fetch_sub(&target->base->present_refs,1);
        atomic_fetch_sub(&P.ct[i]->present_refs,1);
        pthread_mutex_unlock(&g_gbmu);
    }
}
