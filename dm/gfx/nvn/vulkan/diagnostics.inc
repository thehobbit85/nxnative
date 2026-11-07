// Live controls share the present/readback queue. Request threads only enqueue
// screenshots. Resource lookup stays locked until its GPU copy has completed.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "../../../third_party/flip/tool/stb_image_write.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include <errno.h>

#define DIAGNOSTIC_SHOTS 8
typedef struct { uint64_t frame; char path[1024]; } diagnostic_shot;
static diagnostic_shot g_shots[DIAGNOSTIC_SHOTS];
static unsigned g_nshots;
static pthread_mutex_t g_diagnostic_mu=PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint8_t *data;
    size_t row, bytes;
    uint32_t width, height, slices;
    VkFormat format;
} diagnostic_image;

static int diagnostic_read_locked(vtex *texture,uint32_t level,uint32_t layer,uint32_t slices,
                                  VkFormat format,diagnostic_image *out) {
    int bytes=rt_bpp(format);
    VkImageAspectFlags aspect=texture->aspect;
    if (aspect&VK_IMAGE_ASPECT_DEPTH_BIT) aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!bytes || bytes!=rt_bpp(texture->f.f) || texture->samples!=VK_SAMPLE_COUNT_1_BIT) {
        LOG("[RED] vk: diagnostic texture %llu format %d samples %u needs a conversion",(unsigned long long)texture->serial,format,texture->samples);
        return 0;
    }
    if (level>=texture->levels) return 0;
    uint32_t width=texture->w>>level, height=texture->h>>level;
    if (!width) width=1;
    if (!height) height=1;
    uint32_t layers=texture->target==NVN_TEXTURE_TARGET_3D ? texture->d>>level : texture->layers;
    if (!layers) layers=1;
    if (layer>=layers || !slices || slices>layers-layer) return 0;
    size_t row=(size_t)width*bytes, size=row*height;
    if (size>SIZE_MAX/slices) return 0;
    size*=slices;
    uint8_t *data=malloc(size);
    if (!data) return 0;
    VkDeviceMemory memory;
    void *mapped;
    VkBuffer buffer=buf_new(size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&memory,&mapped);
    VkBufferImageCopy region={.imageSubresource={aspect,level,0,1},.imageExtent={width,height,1}};
    if (texture->target==NVN_TEXTURE_TARGET_3D) {
        region.imageOffset.z=layer;
        region.imageExtent.depth=slices;
    } else {
        region.imageSubresource.baseArrayLayer=layer;
        region.imageSubresource.layerCount=slices;
    }
    VkCommandBuffer command=once_begin();
    barrier_all(command);
    vkCmdCopyImageToBuffer(command,texture->img,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&region);
    once_end(command);
    memcpy(data,mapped,size);
    *out=(diagnostic_image){data,row,size,width,height,slices,format};
    vkDestroyBuffer(g_dev,buffer,NULL);
    vkFreeMemory(g_dev,memory,NULL);
    return 1;
}

static int diagnostic_read(uint64_t serial,int all_slices,diagnostic_image *out) {
    if (!g_dev) return 0;
    mtl_finish();
    pthread_mutex_lock(&g_rmu);
    vtex *texture=tex_find(serial);
    int ok=0;
    if (texture) {
        uint32_t slices=all_slices ? (texture->target==NVN_TEXTURE_TARGET_3D ? texture->d : texture->layers) : 1;
        ok=diagnostic_read_locked(texture,0,0,slices,texture->f.f,out);
    } else LOG("[RED] vk: diagnostic texture %llu has no host image",(unsigned long long)serial);
    pthread_mutex_unlock(&g_rmu);
    return ok;
}

static int diagnostic_write(const char *path,const char *header,const void *data,size_t bytes) {
    char temporary[1100];
    if (!path || !*path || snprintf(temporary,sizeof temporary,"%s.tmp.XXXXXX",path)>=(int)sizeof temporary) return 0;
    int descriptor=mkstemp(temporary);
    if (descriptor<0) { LOG("[RED] vk: diagnostic output %s failed (%s)",path,strerror(errno));return 0; }
    FILE *file=fdopen(descriptor,"wb");
    if (!file) { close(descriptor);unlink(temporary);return 0; }
    int ok=(!header || fputs(header,file)>=0) && fwrite(data,1,bytes,file)==bytes;
    if (fclose(file)) ok=0;
    if (ok) ok=!rename(temporary,path);
    if (ok) chmod(path,0644);
    else { LOG("[RED] vk: diagnostic output %s failed (%s)",path,strerror(errno));unlink(temporary); }
    return ok;
}

static void diagnostic_write_png(const char *path,const uint8_t *rgba,uint32_t width,uint32_t height) {
    int bytes=0;
    uint8_t *png=stbi_write_png_to_mem(rgba,width*4,width,height,4,&bytes);
    if (png && diagnostic_write(path,NULL,png,bytes)) LOG("vk: screenshot written to %s (%ux%u)",path,width,height);
    free(png);
}

#include "diagnostic_present.inc"

static void diagnostic_png(const tex_t *texture,const int crop[4],const char *path) {
    if (!texture || !g_dev) return;
    const char *setting=getenv("DM_NVN_SHOT_OUT");int ow=0,oh=0;char sep=0,extra=0;
    int scaled=setting && sscanf(setting,"%d%c%d%c",&ow,&sep,&oh,&extra)==3 && sep=='x' && ow>0 && oh>0;
    diagnostic_image image={0};int converted=0,ok=0;
    mtl_finish();pthread_mutex_lock(&g_rmu);
    vtex *source=tex_find(texture->serial);
    if (source) {
        int rgba=source->f.f==VK_FORMAT_R8G8B8A8_UNORM || source->f.f==VK_FORMAT_R8G8B8A8_SRGB ||
            source->f.f==VK_FORMAT_B8G8R8A8_UNORM || source->f.f==VK_FORMAT_B8G8R8A8_SRGB;
        converted=scaled || !rgba;
        if (converted) {
            uint32_t w=scaled ? (uint32_t)ow : crop && crop[2]>0 ? (uint32_t)crop[2] : source->w;
            uint32_t h=scaled ? (uint32_t)oh : crop && crop[3]>0 ? (uint32_t)crop[3] : source->h;
            ok=diagnostic_render_locked(source,crop,w,h,scaled,&image);
        } else ok=diagnostic_read_locked(source,0,0,1,source->f.f,&image);
    }
    pthread_mutex_unlock(&g_rmu);
    if (!ok) return;
    if (converted) crop=NULL; // The present shader already applied crop and origin.
    int bgr=image.format==VK_FORMAT_B8G8R8A8_UNORM || image.format==VK_FORMAT_B8G8R8A8_SRGB;
    uint32_t x0=0,y0=0,width=image.width,height=image.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0]<0 || crop[1]<0 || crop[2]<=0 || crop[3]<=0 ||
            (uint64_t)crop[0]+crop[2]>width || (uint64_t)crop[1]+crop[3]>height) {
            LOG("[RED] vk: screenshot crop is outside the image");free(image.data);return;
        }
        x0=crop[0];y0=crop[1];width=crop[2];height=crop[3];
    }
    if ((uint64_t)width*height*4>INT_MAX) { free(image.data);return; }
    uint8_t *rgba=malloc((size_t)width*height*4);
    if (!rgba) { free(image.data);return; }
    for (uint32_t y=0;y<height;y++) for (uint32_t x=0;x<width;x++) {
        uint32_t source_y=y0+(!converted && g_origin_ll ? height-1-y : y);
        const uint8_t *source=image.data+source_y*image.row+(x0+x)*4;
        uint8_t *pixel=rgba+((size_t)y*width+x)*4;
        pixel[0]=source[bgr ? 2 : 0];pixel[1]=source[1];pixel[2]=source[bgr ? 0 : 2];pixel[3]=255;
    }
    diagnostic_write_png(path,rgba,width,height);
    free(rgba);free(image.data);
}

void dm_gfx_shot_at(long frames,const char *path) {
    if (!path || !*path || strlen(path)>=sizeof g_shots[0].path) { LOG("[RED] vk: invalid screenshot path");return; }
    pthread_mutex_lock(&g_diagnostic_mu);
    if (g_nshots==DIAGNOSTIC_SHOTS) LOG("[RED] vk: screenshot queue is full, dropping %s",path);
    else {
        diagnostic_shot *request=&g_shots[g_nshots++];
        request->frame=atomic_load(&g_presents)+(uint64_t)(frames>0 ? frames : 0);
        strcpy(request->path,path);
    }
    pthread_mutex_unlock(&g_diagnostic_mu);
}

#include "diagnostic_targets.inc"

static void diagnostic_present(const tex_t *texture,uint64_t frame,const int crop[4]) {
    diagnostic_shot due[DIAGNOSTIC_SHOTS];unsigned count=0,keep=0;
    pthread_mutex_lock(&g_diagnostic_mu);
    for (unsigned i=0;i<g_nshots;i++) {
        if (g_shots[i].frame<=frame) due[count++]=g_shots[i];
        else g_shots[keep++]=g_shots[i];
    }
    g_nshots=keep;
    pthread_mutex_unlock(&g_diagnostic_mu);
    for (unsigned i=0;i<count;i++) diagnostic_png(texture,crop,due[i].path);
    diagnostic_dump_targets(frame);
}

void dm_gfx_texraw(uint64_t serial,const char *path) {
    diagnostic_image image={0};
    if (!diagnostic_read(serial,1,&image)) return;
    // The target equivalent reports VkFormat and tightly packed rows. Extra
    // slices follow consecutively, including every cube face and 3D plane.
    char header[128];
    if (image.slices>1) snprintf(header,sizeof header,"%u %u %u %zu %u\n",image.width,image.height,image.format,image.row,image.slices);
    else snprintf(header,sizeof header,"%u %u %u %zu\n",image.width,image.height,image.format,image.row);
    if (diagnostic_write(path,header,image.data,image.bytes))
        LOG("[texraw] ser=%llu %ux%u Vulkan format %u row %zu slices %u -> %s",(unsigned long long)serial,image.width,image.height,image.format,image.row,image.slices,path);
    free(image.data);
}

void dm_gfx_texlist(int target) {
    pthread_mutex_lock(&g_rmu);
    for (int i=1;i<g_ntreg;i++) {
        const treg_t *registration=&g_treg[i];const tex_t *texture=&registration->t;
        if (!registration->ok || texture->d.target!=target) continue;
        LOG("[texlist] tid=%d ser=%llu target %d %ux%ux%u format %#x levels %u flags %#x storage %p size %#llx block height/depth log2 %u/%u",
            i,(unsigned long long)texture->serial,texture->d.target,texture->d.width,texture->d.height,texture->d.depth,
            texture->d.format,texture->d.levels,texture->d.flags,(void *)texture->mem,(unsigned long long)texture->L.size,texture->L.tileH,texture->L.tileD);
    }
    pthread_mutex_unlock(&g_rmu);
}
