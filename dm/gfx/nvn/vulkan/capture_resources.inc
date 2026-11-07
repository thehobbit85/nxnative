static void cap_iface(const prog_t *program,uint64_t out[6]) {
    memset(out,0,6*sizeof *out);
    for (unsigned stage=0;stage<program->nstage && stage<6;stage++) {
        uint8_t key[8],digest[32];
        if (!prog_key(program,stage,key)) continue;
        shader_t *shader=shader_get(key);
        if (!shader || !shader->mod) continue;
        CC_SHA256_CTX hash;CC_SHA256_Init(&hash);
        uint32_t header[]={CAP_VK_IFACE,shader->guest_stage,shader->nb,shader->vin_mask,
            shader->pack_locations,shader->tg[0],shader->tg[1],shader->tg[2]};
        CC_SHA256_Update(&hash,header,sizeof header);
        CC_SHA256_Update(&hash,shader->vin,sizeof shader->vin);
        for (int i=0;i<shader->nb;i++) {
            const sbind *b=&shader->b[i];
            const int64_t fields[]={b->binding,b->count,b->storage_image,b->logical_count,
                b->type,b->kind,b->skind,b->read_only,b->table,b->pool,b->texel_atomic,
                b->multisample,b->depth_image,b->slot,b->reg,b->sreg,b->off,b->soff,
                b->view,b->num,b->driver_sampler,b->twin.texture.reg,b->twin.texture.off,
                b->twin.sampler.reg,b->twin.sampler.off,b->shape.fetch1d,b->shape.fetch1d_size};
            CC_SHA256_Update(&hash,fields,sizeof fields);
        }
        CC_SHA256_Final(digest,&hash);
        memcpy(&out[shader->guest_stage],digest,8);
        out[shader->guest_stage]|=1;
    }
}

// One plane at a time keeps depth and stencil transfer sizes independent.
// Blocks stay encoded when the native image uses a compressed format.
static int cap_image_read(vtex *texture,uint32_t level,uint32_t slice,uint32_t plane,
                          cap_tsub *sub,cap_buf *pixels) {
    uint32_t width=CAP_MAX(texture->w>>level,1u),height=CAP_MAX(texture->h>>level,1u);
    uint32_t depth=texture->target==NVN_TEXTURE_TARGET_3D ? CAP_MAX(texture->d>>level,1u) : 1;
    int bw=1,bh=1,bytes=rt_bpp(texture->f.f);
    VkImageAspectFlags aspect=texture->aspect;
    if (plane) { bytes=1;aspect=VK_IMAGE_ASPECT_STENCIL_BIT; }
    else if (aspect&VK_IMAGE_ASPECT_DEPTH_BIT) {
        aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
        if (texture->f.f==VK_FORMAT_D16_UNORM_S8_UINT) bytes=2;
    }
    if (!bytes) bytes=nvn_fmt_bpb(texture->nvn_fmt,&bw,&bh);
    if (!bytes || bw<1 || bh<1) return 0;
    uint64_t row=((uint64_t)width+bw-1)/bw*bytes,img=row*((height+bh-1)/bh);
    if (!img || img>SIZE_MAX/depth) return 0;
    uint64_t size=img*depth;
    VkDeviceMemory memory;void *mapped;
    VkBuffer buffer=buf_new(size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&memory,&mapped);
    VkBufferImageCopy copy={.imageSubresource={aspect,level,slice,1},.imageExtent={width,height,depth}};
    VkCommandBuffer command=once_begin();
    barrier_all(command);
    vkCmdCopyImageToBuffer(command,texture->img,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
    once_end(command);
    cap_buf_put(pixels,mapped,size);
    *sub=(cap_tsub){level,slice,width,height,depth,plane,row,img,size};
    vkDestroyBuffer(g_dev,buffer,NULL);vkFreeMemory(g_dev,memory,NULL);
    return 1;
}

static void cap_tex_read(const tex_t *t,int rec) {
    (void)rec;
    if (g_cap_no_read || !t || !t->serial || t->magic!=MAGIC('T','E','X',0) || nvn_hm_get(&g_cap_read,&t->serial)) return;
    nvn_hm_put(&g_cap_read,&t->serial,(void *)1);
    pthread_mutex_lock(&g_rmu);
    vtex *texture=tex_find(t->serial);
    int cpu=cpu_written(t->serial) || (texture && texture->uploaded && !texture->gpu);
    cap_tknd kind={*t,CAP_TK_LIVE|(cpu ? CAP_TK_CPU : 0),0};
    cap_tex_enc(&kind.t,"texture kind");cap_buf_put(&g_cap_kind,&kind,sizeof kind);
    if (!texture || (texture->uploaded && !texture->gpu)) { pthread_mutex_unlock(&g_rmu);return; }
    if (texture->samples!=VK_SAMPLE_COUNT_1_BIT) {
        g_cap_tex_skip++;cap_fail("multisample image contents require sample-preserving capture");
        pthread_mutex_unlock(&g_rmu);return;
    }
    uint32_t planes=texture->aspect==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT) ? 2 : 1;
    uint32_t slices=texture->target==NVN_TEXTURE_TARGET_3D ? 1 : texture->layers;
    cap_tpix header={*t,texture->f.f,texture->target==NVN_TEXTURE_TARGET_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        texture->w,texture->h,texture->d,texture->levels,slices,texture->samples,planes*texture->levels*slices,0};
    cap_buf subs={0},pixels={0},chunk={0};
    int ok=1;
    for (uint32_t plane=0;plane<planes && ok;plane++)
        for (uint32_t level=0;level<texture->levels && ok;level++)
            for (uint32_t slice=0;slice<slices && ok;slice++) {
                cap_tsub sub;
                ok=cap_image_read(texture,level,slice,plane,&sub,&pixels);
                if (ok) cap_buf_put(&subs,&sub,sizeof sub);
            }
    pthread_mutex_unlock(&g_rmu);
    if (ok) {
        cap_tex_enc(&header.t,"texture contents");
        cap_buf_put(&chunk,&header,sizeof header);cap_buf_put(&chunk,subs.p,subs.n);cap_buf_put(&chunk,pixels.p,pixels.n);
        cap_put(&g_cap_w,CAP_TPIX,g_cap_cur_submit,chunk.p,chunk.n,1);
        g_cap_tex_bytes+=pixels.n;g_cap_tex_n++;
    } else { g_cap_tex_skip++;cap_fail("image format cannot be captured"); }
    cap_buf_free(&subs);cap_buf_free(&pixels);cap_buf_free(&chunk);
}

static int cap_digest(const tex_t *texture,uint8_t digest[32],uint64_t *bytes,uint32_t *width,
                      uint32_t *height,uint32_t *format,const char *png,cap_buf *pixels) {
    diagnostic_image image={0};
    if (!texture || !diagnostic_read(texture->serial,0,&image)) return 0;
    CC_SHA256_CTX hash;CC_SHA256_Init(&hash);
    for (size_t offset=0;offset<image.bytes;) {
        size_t count=CAP_MIN(image.bytes-offset,UINT32_MAX);
        CC_SHA256_Update(&hash,image.data+offset,(CC_LONG)count);offset+=count;
    }
    CC_SHA256_Final(digest,&hash);
    if (pixels) cap_buf_put(pixels,image.data,image.bytes);
    *bytes=image.bytes;*width=image.width;*height=image.height;*format=image.format;
    free(image.data);
    if (png) diagnostic_png(texture,NULL,png);
    return 1;
}
