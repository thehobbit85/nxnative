// A dump owns its attachments until their end-of-frame readbacks complete,
// including textures the guest releases before the present thread gets there.
typedef struct {
    vtex *texture;
    uint32_t level,layer;
    VkFormat format;
} diagnostic_target;
static diagnostic_target *g_dump_targets;
static size_t g_dump_count,g_dump_capacity;
static _Atomic uint64_t g_dump_frame;
static char g_dump_path[1024];

void dm_gfx_texdump_next(const char *path) {
    const char *slot=path ? strstr(path,"%d") : NULL;
    if (!slot || strstr(slot+2,"%d") || strlen(path)>=sizeof g_dump_path) {
        LOG("[RED] vk: texdump path requires one %%d placeholder");
        return;
    }
    pthread_mutex_lock(&g_diagnostic_mu);
    for (size_t i=0;i<g_dump_count;i++) atomic_fetch_sub(&g_dump_targets[i].texture->present_refs,1);
    g_dump_count=0;
    strcpy(g_dump_path,path);
    atomic_store(&g_dump_frame,atomic_load(&g_presents)+1);
    pthread_mutex_unlock(&g_diagnostic_mu);
}

static void diagnostic_target_add(vtex *texture,const image_view_key *view,uint32_t layers) {
    for (uint32_t i=0;i<layers;i++) {
        uint32_t layer=view->range.baseArrayLayer+i,level=view->range.baseMipLevel;
        size_t index=0;
        while (index<g_dump_count && (g_dump_targets[index].texture!=texture ||
               g_dump_targets[index].level!=level || g_dump_targets[index].layer!=layer)) index++;
        if (index<g_dump_count) {
            g_dump_targets[index].format=view->format;
            continue;
        }
        if (g_dump_count==g_dump_capacity) {
            size_t capacity=g_dump_capacity ? g_dump_capacity*2 : 32;
            diagnostic_target *targets=realloc(g_dump_targets,capacity*sizeof *targets);
            if (!targets) { LOG("[RED] vk: cannot retain diagnostic attachments");return; }
            g_dump_targets=targets;
            g_dump_capacity=capacity;
        }
        atomic_fetch_add(&texture->present_refs,1);
        g_dump_targets[g_dump_count++]=(diagnostic_target){texture,level,layer,view->format};
    }
}

static void diagnostic_note_targets(const rp_desc *pass) {
    if (!atomic_load(&g_dump_frame)) return;
    pthread_mutex_lock(&g_diagnostic_mu);
    if (atomic_load(&g_dump_frame)!=atomic_load(&g_presents)+1) {
        pthread_mutex_unlock(&g_diagnostic_mu);
        return;
    }
    pthread_mutex_lock(&g_rmu);
    for (int attachment=0;attachment<pass->n;attachment++) {
        if (!pass->cv[attachment]) continue;
        for (unsigned bucket=0;bucket<NHASH;bucket++) for (vtex *texture=g_tex[bucket];texture;texture=texture->next) {
            for (int i=0;i<texture->nviews;i++) {
                const vview *view=&texture->views[i];
                if (view->v!=pass->cv[attachment]) continue;
                uint32_t layers=pass->layers ? pass->layers : 1;
                if (layers>view->key.range.layerCount) layers=view->key.range.layerCount;
                diagnostic_target_add(texture,&view->key,layers);
            }
        }
    }
    pthread_mutex_unlock(&g_rmu);
    pthread_mutex_unlock(&g_diagnostic_mu);
}

static float diagnostic_component(uint32_t bits,unsigned width,unsigned kind) {
    uint32_t maximum=width==32 ? UINT32_MAX : (1u<<width)-1;
    int32_t signed_value=(int32_t)(bits<<(32-width))>>(32-width);
    switch (kind) {
        case 0: return (float)((double)bits/maximum);
        case 1: return fmaxf(-1,(float)((double)signed_value/(maximum>>1)));
        case 2: case 4: return (float)bits;
        case 3: case 5: return (float)signed_value;
        default: {
            if (width==16) return bcdec__half_to_float_quick(bits);
            float value;memcpy(&value,&bits,sizeof value);return value;
        }
    }
}

static int diagnostic_color(VkFormat format,const uint8_t *pixel,float rgba[4]) {
    static const struct { VkFormat first,last; unsigned width,channels,bgr; } families[]={
        {VK_FORMAT_R8_UNORM,VK_FORMAT_R8_SRGB,8,1,0},
        {VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_SRGB,8,2,0},
        {VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_R8G8B8_SRGB,8,3,0},
        {VK_FORMAT_B8G8R8_UNORM,VK_FORMAT_B8G8R8_SRGB,8,3,1},
        {VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB,8,4,0},
        {VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_B8G8R8A8_SRGB,8,4,1},
        {VK_FORMAT_A8B8G8R8_UNORM_PACK32,VK_FORMAT_A8B8G8R8_SRGB_PACK32,8,4,0},
        {VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SFLOAT,16,1,0},
        {VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SFLOAT,16,2,0},
        {VK_FORMAT_R16G16B16_UNORM,VK_FORMAT_R16G16B16_SFLOAT,16,3,0},
        {VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT,16,4,0},
        {VK_FORMAT_R32_UINT,VK_FORMAT_R32_SFLOAT,32,1,0},
        {VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32_SFLOAT,32,2,0},
        {VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32_SFLOAT,32,3,0},
        {VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32B32A32_SFLOAT,32,4,0},
    };
    rgba[0]=rgba[1]=rgba[2]=0;rgba[3]=1;
    for (unsigned i=0;i<sizeof families/sizeof *families;i++) {
        if (format<families[i].first || format>families[i].last) continue;
        unsigned width=families[i].width,kind=format-families[i].first;
        int srgb=width==8 && kind==6;
        if (srgb) kind=0;
        if (width==32) kind+=4;
        for (unsigned channel=0;channel<families[i].channels;channel++) {
            uint32_t bits=0;
            memcpy(&bits,pixel+channel*(width/8),width/8);
            unsigned output=families[i].bgr && channel<3 ? 2-channel : channel;
            float value=diagnostic_component(bits,width,kind);
            if (srgb && output<3) value=value<=.04045f ? value/12.92f : powf((value+.055f)/1.055f,2.4f);
            rgba[output]=value;
        }
        return 1;
    }
    uint32_t bits=0;
    int bytes=rt_bpp(format);
    if (bytes<1 || bytes>4) return 0;
    memcpy(&bits,pixel,bytes);
    if (format==VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
        rgba[0]=bcdec__half_to_float_quick((bits&2047)<<4);
        rgba[1]=bcdec__half_to_float_quick(((bits>>11)&2047)<<4);
        rgba[2]=bcdec__half_to_float_quick((bits>>22)<<5);
        return 1;
    }
    if (format==VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) {
        for (unsigned channel=0;channel<3;channel++) rgba[channel]=ldexpf((bits>>(9*channel))&511,(int)(bits>>27)-24);
        return 1;
    }
    static const struct { VkFormat format; unsigned widths[4],shifts[4]; } packed[]={
        {VK_FORMAT_A4B4G4R4_UNORM_PACK16,{4,4,4,4},{0,4,8,12}},
        {VK_FORMAT_A1R5G5B5_UNORM_PACK16,{5,5,5,1},{10,5,0,15}},
        {VK_FORMAT_R5G6B5_UNORM_PACK16,{5,6,5,0},{11,5,0,0}},
        {VK_FORMAT_B5G6R5_UNORM_PACK16,{5,6,5,0},{0,5,11,0}},
        {VK_FORMAT_B5G5R5A1_UNORM_PACK16,{5,5,5,1},{1,6,11,0}},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32,{10,10,10,2},{0,10,20,30}},
        {VK_FORMAT_A2B10G10R10_UINT_PACK32,{10,10,10,2},{0,10,20,30}},
    };
    for (unsigned i=0;i<sizeof packed/sizeof *packed;i++) {
        if (format!=packed[i].format) continue;
        for (unsigned channel=0;channel<4;channel++) {
            unsigned width=packed[i].widths[channel];
            if (!width) continue;
            uint32_t value=(bits>>packed[i].shifts[channel])&((1u<<width)-1);
            rgba[channel]=diagnostic_component(value,width,format==VK_FORMAT_A2B10G10R10_UINT_PACK32 ? 4 : 0);
        }
        return 1;
    }
    return 0;
}

static void diagnostic_target_png(const diagnostic_target *target,const char *pattern) {
    diagnostic_image image={0};
    pthread_mutex_lock(&g_rmu);
    int ok=diagnostic_read_locked(target->texture,target->level,target->layer,1,target->format,&image);
    pthread_mutex_unlock(&g_rmu);
    if (!ok) return;
    if ((uint64_t)image.width*image.height*4>INT_MAX) { free(image.data);return; }
    uint8_t *rgba=malloc((size_t)image.width*image.height*4);
    if (!rgba) { free(image.data);return; }
    unsigned bytes=rt_bpp(image.format);
    for (size_t i=0;i<(size_t)image.width*image.height;i++) {
        float color[4];
        if (!diagnostic_color(image.format,image.data+i*bytes,color)) { ok=0;break; }
        for (unsigned channel=0;channel<3;channel++) rgba[4*i+channel]=lroundf(fminf(1,fmaxf(0,color[channel]))*255);
        rgba[4*i+3]=255;
    }
    if (ok) {
        char path[1100];
        const char *slot=strstr(pattern,"%d");
        int length=snprintf(path,sizeof path,"%.*s%llu-%u-%u%s",(int)(slot-pattern),pattern,
            (unsigned long long)target->texture->serial,target->level,target->layer,slot+2);
        if (length>0 && length<(int)sizeof path) diagnostic_write_png(path,rgba,image.width,image.height);
    } else LOG("[RED] vk: texdump conversion for Vulkan format %u is unavailable",image.format);
    free(rgba);free(image.data);
}

static void diagnostic_dump_targets(uint64_t frame) {
    if (!atomic_load(&g_dump_frame)) return;
    pthread_mutex_lock(&g_diagnostic_mu);
    uint64_t requested=atomic_load(&g_dump_frame);
    if (!requested || requested>frame) { pthread_mutex_unlock(&g_diagnostic_mu);return; }
    diagnostic_target *targets=g_dump_targets;
    size_t count=g_dump_count;
    char path[sizeof g_dump_path];
    strcpy(path,g_dump_path);
    g_dump_targets=NULL;g_dump_count=g_dump_capacity=0;
    atomic_store(&g_dump_frame,0);
    pthread_mutex_unlock(&g_diagnostic_mu);
    mtl_finish();
    for (size_t i=0;i<count;i++) {
        diagnostic_target_png(&targets[i],path);
        atomic_fetch_sub(&targets[i].texture->present_refs,1);
    }
    free(targets);
    LOG("[texdump] frame %llu inspected %zu color subresources",(unsigned long long)frame,count);
}
