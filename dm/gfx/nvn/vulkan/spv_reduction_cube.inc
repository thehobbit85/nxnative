// Cube face coordinates follow Vulkan's face/derivative selection tables.
// Reconstruct a direction for every filter tap; the point cube sampler then
// addresses the adjoining face at an edge, preserving seamless min/max taps.
// At a corner the diagonal ray selects one of the three incident corner texels.
// All three are already in the footprint, so this has the same extrema as the
// conventional diagonal tap formed by averaging those three texels.
typedef struct { uint32_t zface,yface,sign,major,s,t,uv; } reduction_cube;
static uint32_t reduction_cube_select(reduction_module *m,const reduction_cube *cube,
                                      uint32_t x,uint32_t y,uint32_t z) {
    uint32_t xy=REDUCTION_VALUE(m,169,m->floating,cube->yface,y,x);
    return REDUCTION_VALUE(m,169,m->floating,cube->zface,z,xy);
}
static void reduction_cube_axes(reduction_module *m,const reduction_cube *cube,uint32_t value,uint32_t axes[3]) {
    uint32_t x=reduction_component(m,value,0),y=reduction_component(m,value,1),z=reduction_component(m,value,2);
    uint32_t sx=REDUCTION_VALUE(m,133,m->floating,cube->sign,x);
    uint32_t sz=REDUCTION_VALUE(m,133,m->floating,cube->sign,z);
    uint32_t ny=REDUCTION_VALUE(m,127,m->floating,y),nsz=REDUCTION_VALUE(m,127,m->floating,sz);
    axes[0]=reduction_cube_select(m,cube,nsz,x,sx);
    axes[1]=reduction_cube_select(m,cube,ny,sz,ny);
    axes[2]=REDUCTION_VALUE(m,133,m->floating,cube->sign,reduction_cube_select(m,cube,x,y,z));
}
static reduction_cube reduction_cube_project(reduction_module *m,uint32_t coordinate) {
    reduction_cube cube={0};
    uint32_t x=reduction_component(m,coordinate,0),y=reduction_component(m,coordinate,1),z=reduction_component(m,coordinate,2);
    uint32_t ax=REDUCTION_EXT(m,m->floating,4,x),ay=REDUCTION_EXT(m,m->floating,4,y),az=REDUCTION_EXT(m,m->floating,4,z);
    uint32_t maximum=REDUCTION_EXT(m,m->floating,40,ax,ay);
    cube.zface=REDUCTION_VALUE(m,190,m->code.boolean,az,maximum);
    cube.yface=REDUCTION_VALUE(m,190,m->code.boolean,ay,ax);
    uint32_t major=reduction_cube_select(m,&cube,x,y,z);
    uint32_t positive=REDUCTION_VALUE(m,190,m->code.boolean,major,reduction_float(m,0));
    cube.sign=REDUCTION_VALUE(m,169,m->floating,positive,reduction_float(m,1),reduction_float(m,-1));
    uint32_t axes[3];reduction_cube_axes(m,&cube,coordinate,axes);
    cube.major=axes[2];
    cube.s=REDUCTION_VALUE(m,136,m->floating,axes[0],cube.major);
    cube.t=REDUCTION_VALUE(m,136,m->floating,axes[1],cube.major);
    uint32_t half=reduction_float(m,.5f);
    uint32_t s=REDUCTION_VALUE(m,133,m->floating,cube.s,half),t=REDUCTION_VALUE(m,133,m->floating,cube.t,half);
    s=REDUCTION_VALUE(m,129,m->floating,s,half);t=REDUCTION_VALUE(m,129,m->floating,t,half);
    cube.uv=REDUCTION_VALUE(m,80,m->float2,s,t);
    return cube;
}
static uint32_t reduction_cube_direction(reduction_module *m,const reduction_cube *cube,uint32_t uv) {
    uint32_t one=reduction_float(m,1),two=reduction_float(m,2);
    uint32_t s=REDUCTION_VALUE(m,133,m->floating,reduction_component(m,uv,0),two);
    uint32_t t=REDUCTION_VALUE(m,133,m->floating,reduction_component(m,uv,1),two);
    s=REDUCTION_VALUE(m,131,m->floating,s,one);t=REDUCTION_VALUE(m,131,m->floating,t,one);
    uint32_t ss=REDUCTION_VALUE(m,133,m->floating,cube->sign,s),st=REDUCTION_VALUE(m,133,m->floating,cube->sign,t);
    uint32_t nt=REDUCTION_VALUE(m,127,m->floating,t),nss=REDUCTION_VALUE(m,127,m->floating,ss);
    uint32_t x=reduction_cube_select(m,cube,cube->sign,s,ss);
    uint32_t y=reduction_cube_select(m,cube,nt,cube->sign,nt);
    uint32_t z=reduction_cube_select(m,cube,nss,st,cube->sign);
    return REDUCTION_VALUE(m,80,m->float3,x,y,z);
}
static uint32_t reduction_cube_gradient(reduction_module *m,const reduction_cube *cube,uint32_t derivative) {
    uint32_t axes[3];reduction_cube_axes(m,cube,derivative,axes);
    uint32_t ds=REDUCTION_VALUE(m,133,m->floating,cube->s,axes[2]);
    uint32_t dt=REDUCTION_VALUE(m,133,m->floating,cube->t,axes[2]);
    ds=REDUCTION_VALUE(m,131,m->floating,axes[0],ds);dt=REDUCTION_VALUE(m,131,m->floating,axes[1],dt);
    uint32_t scale=REDUCTION_VALUE(m,136,m->floating,reduction_float(m,.5f),cube->major);
    ds=REDUCTION_VALUE(m,133,m->floating,ds,scale);dt=REDUCTION_VALUE(m,133,m->floating,dt,scale);
    return REDUCTION_VALUE(m,80,m->float2,ds,dt);
}
