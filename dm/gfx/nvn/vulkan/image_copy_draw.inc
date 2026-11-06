// A sampled copy can write a renderable destination even when that format
// lacks BLIT_DST. Depth resolves read sample zero; color blits sample the
// guest rectangle with the requested filter and mirror axes.
typedef struct { int32_t src[4], dst[4], linear; } copy_draw_params;

static VkPipelineLayout image_draw_layout(shader_t *fragment, uint32_t push_bytes) {
    VkDescriptorSetLayoutBinding binding={0,descriptor_type[fragment->b[0].type],1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo set={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,1,&binding};
    VKCHECK(vkCreateDescriptorSetLayout(g_dev,&set,NULL,&fragment->dsl));
    VkPushConstantRange push={VK_SHADER_STAGE_FRAGMENT_BIT,0,push_bytes};
    VkPipelineLayoutCreateInfo info={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,1,&fragment->dsl,1,&push};
    VkPipelineLayout layout;
    VKCHECK(vkCreatePipelineLayout(g_dev,&info,NULL,&layout));
    return layout;
}
static VkDescriptorSet image_draw_set(shader_t *fragment, VkImageView source, VkSampler sampler) {
    VkDescriptorSet set=allocate_shader_set(fragment);
    VkDescriptorImageInfo image={sampler,source,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,set,0,0,1,descriptor_type[fragment->b[0].type],&image};
    vkUpdateDescriptorSets(g_dev,1,&write,0,NULL);
    return set;
}

static int draw_image_slice(const p_copy_t2t *c, vtex *src, vtex *dst, uint32_t sl, uint32_t sly,
                            uint32_t dl, uint32_t dly, int linear) {
    static const uint32_t resolve_code[] =
#include "resolve_depth.frag.inc"
    ;
    static const uint32_t color_code[] =
#include "blit_color.frag.inc"
    ;
    static shader_t depth_shader = {.key="resolve-depth",.stage=1,.nb=1,.b={{.type=BT_TEX,.count=1}}};
    static shader_t color_shader = {.key="blit-color",.stage=1,.nb=1,.b={{.type=BT_TEX,.count=1}}};
    static VkPipelineLayout layout;
    if (!layout) {
        layout=image_draw_layout(&color_shader,sizeof(copy_draw_params));
        depth_shader.dsl=color_shader.dsl;
    }
    shader_t *fragment = dst->f.depth ? clear_shader(&depth_shader,resolve_code,sizeof resolve_code)
                                      : clear_shader(&color_shader,color_code,sizeof color_code);
    view_t view={.set_levels=1,.base_level=sl,.nlevels=1,.set_layers=1,.min_layer=sly,.nlayers=1,
                 .set_swizzle=1,.swz={2,3,4,5}};
    VkImageView source=tex_view(src,c->src,&view,1,VK_IMAGE_VIEW_TYPE_2D,VIEW_SAMPLED);
    view.base_level=dl; view.min_layer=dly;
    VkImageView destination=tex_view(dst,c->dst,&view,1,VK_IMAGE_VIEW_TYPE_2D,VIEW_ATTACHMENT);
    if (!source || !destination) return 0;
    VkExtent2D size={dst->w >> dl ? dst->w >> dl : 1,dst->h >> dl ? dst->h >> dl : 1};
    rp_desc pass={.w=size.width,.h=size.height,.layout=VK_IMAGE_LAYOUT_GENERAL};
    pso_key k={.vs=clear_vertex(),.fs=fragment,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES};
    if (dst->f.depth) {
        pass.dv=destination; pass.df=k.df=dst->f.f; pass.stencil=k.stencil=dst->f.stencil;
        pass.dl=pass.sl=VK_ATTACHMENT_LOAD_OP_LOAD;
        k.ds.dt=k.ds.dw=1; k.ds.df=NVN_COMPARE_FUNC_ALWAYS;
    } else {
        pass.n=k.nc=1; pass.cv[0]=destination; pass.cf[0]=k.cf[0]=dst->f.f;
        pass.cl[0]=VK_ATTACHMENT_LOAD_OP_LOAD; k.chmask.mask=15;
    }
    VkPipeline pipeline=pso_get(&k,layout);
    if (!pipeline) return 0;
    VkDescriptorSet set=image_draw_set(fragment,source,VK_NULL_HANDLE);
    barrier_all(C->cb);
    begin_tracked_pass(&pass);
    VkRect2D rect={{c->dr.xoffset,c->dr.yoffset},{(uint32_t)c->dr.width,(uint32_t)c->dr.height}};
    clear_draw_state(pipeline,rect,size,0);
    vkCmdBindDescriptorSets(C->cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);
    if (dst->f.depth) {
        int32_t offset[]={c->sr.xoffset-c->dr.xoffset,c->sr.yoffset-c->dr.yoffset};
        vkCmdPushConstants(C->cb,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof offset,offset);
    } else {
        copy_draw_params p={{c->sr.xoffset,c->sr.yoffset,c->sr.width,c->sr.height},
                            {c->dr.xoffset,c->dr.yoffset,c->dr.width,c->dr.height},linear};
        if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { p.src[0]+=p.src[2]; p.src[2]=-p.src[2]; }
        if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { p.src[1]+=p.src[3]; p.src[3]=-p.src[3]; }
        vkCmdPushConstants(C->cb,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof p,&p);
    }
    vkCmdDraw(C->cb,3,1,0,0); rp_end(C->cb); barrier_all(C->cb);
    return 1;
}
