// NVN2's temporal upscaler: one metadata pass per input pixel and one SGSR2
// reconstruction per output pixel. Guest lists can evaluate multiple networks
// and outputs before a submission ends; intermediate images retire at a fence.
typedef struct {
    uint64_t net, use;
    VkFormat format[4];
    uint32_t size[4], flags;
    vtex *history[2];
    int next, valid;
    float pre_exposure;
} temporal_network;
static temporal_network temporal_networks[4];
static uint64_t temporal_use;
static const tex_t temporal_desc={.d={.target=NVN_TEXTURE_TARGET_2D}};

static temporal_network *temporal_slot(uint64_t net) {
    temporal_network *oldest=&temporal_networks[0];
    for (int i=0;i<4;i++) {
        temporal_network *e=&temporal_networks[i];
        if (e->use && e->net==net) return e;
        if (e->use<oldest->use) oldest=e;
    }
    return oldest;
}
static vtex *temporal_surface(vtex **slot, uint32_t w, uint32_t h) {
    if (*slot && (*slot)->w==w && (*slot)->h==h) return *slot;
    vtex *next=sampled_scratch(fmt_of(NVN_FORMAT_RGBA16F),VK_SAMPLE_COUNT_1_BIT,w,h,1,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!next) return NULL;
    if (*slot) tex_free(*slot);
    return *slot=next;
}
static VkImageView temporal_view(vtex *image, const tex_t *texture, int purpose) {
    view_t view={.set_levels=1,.nlevels=1,.set_swizzle=1,.swz={2,3,4,5}};
    return tex_view(image,texture,&view,1,VK_IMAGE_VIEW_TYPE_2D,purpose);
}
static VkImageView temporal_input(const tex_t *texture, int x, int y, int w, int h, VkFormat *format) {
    vtex *image=texture ? tex_get(texture,0) : NULL;
    if (!image || image->target!=NVN_TEXTURE_TARGET_2D || image->samples!=1 ||
        clear_color_type(image->f.f)!=CLEAR_FLOAT || !lvl_fits(image,0,0,1,x,y,0,w,h,1)) return VK_NULL_HANDLE;
    *format=image->f.f;
    return temporal_view(image,texture,VIEW_SAMPLED);
}
static int temporal_pass(shader_t *shader, VkPipelineLayout layout, const VkImageView inputs[],
                         vtex *output, const void *params, uint32_t bytes) {
    VkImageView target=temporal_view(output,&temporal_desc,VIEW_ATTACHMENT);
    if (!target) return 0;
    pso_key key={.vs=clear_vertex(),.fs=shader,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.chmask.mask=15,
        .nc=1,.cf={output->f.f}};
    VkPipeline pipeline=pso_get(&key,layout);
    if (!pipeline) return 0;
    VkDescriptorSet set=allocate_shader_set(shader);
    VkDescriptorImageInfo images[shader->nb]; VkWriteDescriptorSet writes[shader->nb];
    for (int i=0;i<shader->nb;i++) {
        images[i]=(VkDescriptorImageInfo){VK_NULL_HANDLE,inputs[i],VK_IMAGE_LAYOUT_GENERAL};
        writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,set,i,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,&images[i]};
    }
    vkUpdateDescriptorSets(g_dev,shader->nb,writes,0,NULL);
    rp_desc pass={.w=output->w,.h=output->h,.n=1,.cv={target},.cf={output->f.f},
        .cl={VK_ATTACHMENT_LOAD_OP_DONT_CARE},.layout=VK_IMAGE_LAYOUT_GENERAL};
    barrier_all(C->cb);
    begin_tracked_pass(&pass);
    VkExtent2D size={output->w,output->h};
    clear_draw_state(pipeline,(VkRect2D){{0,0},size},size,0);
    vkCmdBindDescriptorSets(C->cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);
    vkCmdPushConstants(C->cb,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,bytes,params);
    vkCmdDraw(C->cb,3,1,0,0);
    rp_end(C->cb); barrier_all(C->cb);
    return 1;
}
static int temporal_eval(const p_dlss *job, const p_copy_t2t *copy, vtex *src, vtex *dst) {
    static const uint32_t prepare_code[]=
#include "temporal_prepare.frag.inc"
    ;
    static const uint32_t resolve_code[]=
#include "temporal_resolve.frag.inc"
    ;
    static shader_t prepare={.key="temporal-prepare",.stage=1,.nb=4};
    static shader_t resolve={.key="temporal-resolve",.stage=1,.nb=4};
    static VkPipelineLayout prepare_layout, resolve_layout;
    static vtex *metadata;
    int iw=copy->sr.width, ih=copy->sr.height, ow=copy->dr.width, oh=copy->dr.height;
    temporal_network *e=temporal_slot(job->net);
    int same=e->use && e->net==job->net;
    int old_valid=same && e->valid;
    if (same) e->valid=0;   // a spatial-only or failed frame breaks this network's history
    if (!job->tex[DLSS_DEPTH] || !job->tex[DLSS_MOTION]) return 0;
    VkFormat format[4]={src->f.f,VK_FORMAT_UNDEFINED,VK_FORMAT_UNDEFINED,dst->f.f};
    VkImageView inputs[4];
    inputs[0]=temporal_input(job->tex[DLSS_DEPTH],job->base[DLSS_DEPTH][0],job->base[DLSS_DEPTH][1],iw,ih,&format[1]);
    inputs[1]=temporal_input(job->tex[DLSS_MOTION],job->base[DLSS_MOTION][0],job->base[DLSS_MOTION][1],iw,ih,&format[2]);
    inputs[2]=temporal_view(src,copy->src,VIEW_SAMPLED);
    int auto_expose=(job->flags&DLSS_F_AUTO_EXPOSURE)!=0;
    VkFormat exposure_format;
    inputs[3]=job->tex[DLSS_EXPOSURE] && !auto_expose ?
        temporal_input(job->tex[DLSS_EXPOSURE],0,0,1,1,&exposure_format) : g_dummy_view;
    for (int i=0;i<4;i++) if (!inputs[i]) return 0;
    e->valid=0;   // replacing one image of an evicted pair can still fail on the second
    if (!temporal_surface(&metadata,iw,ih) || !temporal_surface(&e->history[0],ow,oh) ||
        !temporal_surface(&e->history[1],ow,oh)) return 0;
    uint32_t dimensions[]={iw,ih,ow,oh};
    int fresh=!old_valid || memcmp(e->format,format,sizeof format) || memcmp(e->size,dimensions,sizeof dimensions) || e->flags!=job->flags;
    float pre=job->pre_exposure>0 && isfinite(job->pre_exposure) ? job->pre_exposure : 1;
    struct { int32_t size[2], depth[2], motion[2], color[2]; float scale[2]; int32_t reversed, exposure; float pre; }
        a={{iw,ih},{job->base[DLSS_DEPTH][0],job->base[DLSS_DEPTH][1]},
           {job->base[DLSS_MOTION][0],job->base[DLSS_MOTION][1]},{copy->sr.xoffset,copy->sr.yoffset},
           {job->mv[0],job->mv[1]},(job->flags&DLSS_F_DEPTH_INVERTED)!=0,auto_expose ? 2 : job->tex[DLSS_EXPOSURE]!=NULL,pre};
    struct { int32_t size[2], output[2], color[2]; float jitter[2], pre, previous; int32_t reset, reversed, depth[2]; }
        b={{iw,ih},{ow,oh},{copy->sr.xoffset,copy->sr.yoffset},{job->jitter[0],job->jitter[1]},pre,
           fresh ? pre : e->pre_exposure,fresh || job->reset,a.reversed,{a.depth[0],a.depth[1]}};
    if (!prepare_layout) {
        for (int i=0;i<prepare.nb;i++) prepare.b[i]=(sbind){.type=BT_TEX,.count=1};
        for (int i=0;i<resolve.nb;i++) resolve.b[i]=(sbind){.type=BT_TEX,.count=1};
        clear_shader(&prepare,prepare_code,sizeof prepare_code); clear_shader(&resolve,resolve_code,sizeof resolve_code);
        prepare_layout=image_draw_layout(&prepare,sizeof a); resolve_layout=image_draw_layout(&resolve,sizeof b);
    }
    if (!temporal_pass(&prepare,prepare_layout,inputs,metadata,&a,sizeof a)) return 0;
    vtex *next=e->history[e->next];
    VkImageView recon[]={inputs[2],temporal_view(metadata,&temporal_desc,VIEW_SAMPLED),
        b.reset ? g_dummy_view : temporal_view(e->history[1-e->next],&temporal_desc,VIEW_SAMPLED),inputs[0]};
    if (!recon[1] || !recon[2] || !temporal_pass(&resolve,resolve_layout,recon,next,&b,sizeof b)) return 0;
    p_copy_t2t result={.src=&temporal_desc,.dst=copy->dst,.sr={.width=ow,.height=oh,.depth=1},.dr=copy->dr};
    if (!draw_image_slice(&result,next,dst,0,0,0,0,COPY_DRAW_OPAQUE)) return 0;
    e->net=job->net; e->use=++temporal_use; e->valid=1; e->next=1-e->next; e->pre_exposure=pre; e->flags=job->flags;
    memcpy(e->format,format,sizeof format); memcpy(e->size,dimensions,sizeof dimensions);
    return 1;
}
