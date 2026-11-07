// A bounded line of isotropic footprints approximates the anisotropic ellipse.
// The exact kernel is implementation-dependent in Vulkan. Use the longer
// direction derivative for the line and the shorter scale (or major/N) for LOD.
// Explicit-Lod instructions have zero derivatives and retain one footprint.
typedef struct { uint32_t lod,direction,samples; } reduction_footprint;
static reduction_footprint reduction_anisotropic_footprint(reduction_module *m,uint32_t image,uint32_t dx,uint32_t dy,
    unsigned dimensions,unsigned array,const reduction_cube *cube,uint32_t anisotropy) {
    if (cube) { dx=reduction_cube_gradient(m,cube,dx);dy=reduction_cube_gradient(m,cube,dy); }
    uint32_t spatial=dimensions==3 ? m->float3 : m->float2;
    uint32_t zero=texel_constant(&m->code,m->sint,0);if (!zero) m->failed=1;
    uint32_t size=REDUCTION_VALUE(m,103,array || dimensions==3 ? m->int3 : m->int2,image,zero);
    if (array) size=REDUCTION_VALUE(m,79,m->int2,size,size,0,1);
    size=REDUCTION_VALUE(m,111,spatial,size);
    uint32_t x=REDUCTION_VALUE(m,133,spatial,dx,size),y=REDUCTION_VALUE(m,133,spatial,dy,size);
    x=REDUCTION_VALUE(m,148,m->floating,x,x);y=REDUCTION_VALUE(m,148,m->floating,y,y);
    x=REDUCTION_EXT(m,m->floating,31,x);y=REDUCTION_EXT(m,m->floating,31,y);
    uint32_t major=REDUCTION_EXT(m,m->floating,40,x,y),minor=REDUCTION_EXT(m,m->floating,37,x,y);
    uint32_t direction=REDUCTION_VALUE(m,190,m->code.boolean,x,y);
    reduction_footprint result={0};
    result.direction=REDUCTION_VALUE(m,169,spatial,reduction_mask(m,direction,dimensions),dx,dy);
    uint32_t one=reduction_float(m,1),denominator=REDUCTION_EXT(m,m->floating,40,minor,one);
    uint32_t samples=REDUCTION_VALUE(m,136,m->floating,major,denominator);
    samples=REDUCTION_EXT(m,m->floating,9,samples);
    result.samples=REDUCTION_EXT(m,m->floating,43,samples,one,anisotropy);
    uint32_t covered=REDUCTION_VALUE(m,136,m->floating,major,result.samples);
    covered=REDUCTION_EXT(m,m->floating,40,covered,minor);
    result.lod=REDUCTION_EXT(m,m->floating,30,covered);
    return result;
}
static uint32_t reduction_anisotropic_coordinate(reduction_module *m,uint32_t coordinate,const reduction_footprint *footprint,
    unsigned tap,unsigned dimensions,unsigned array,const reduction_cube *cube) {
    uint32_t offset=REDUCTION_VALUE(m,136,m->floating,reduction_float(m,tap+.5f),footprint->samples);
    offset=REDUCTION_VALUE(m,131,m->floating,offset,reduction_float(m,.5f));
    uint32_t spatial=dimensions==3 ? m->float3 : m->float2;
    uint32_t step=REDUCTION_VALUE(m,133,spatial,footprint->direction,reduction_splat(m,spatial,offset,dimensions));
    uint32_t position=cube ? cube->uv : array ? REDUCTION_VALUE(m,79,m->float2,coordinate,coordinate,0,1) : coordinate;
    position=REDUCTION_VALUE(m,129,spatial,position,step);
    if (cube) position=reduction_cube_direction(m,cube,position);
    if (array) {
        uint32_t layer=reduction_component(m,coordinate,cube ? 3 : 2);
        uint32_t x=reduction_component(m,position,0),y=reduction_component(m,position,1);
        position=cube ? REDUCTION_VALUE(m,80,m->float4,x,y,reduction_component(m,position,2),layer) :
            REDUCTION_VALUE(m,80,m->float3,x,y,layer);
    }
    return position;
}
