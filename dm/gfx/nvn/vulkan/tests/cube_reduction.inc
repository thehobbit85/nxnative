// Constant face colors make the seam oracle independent of the selected face:
// each edge touches two faces and each corner three. Opposite channel ordering
// verifies component-wise reduction rather than choosing one whole texel.
static void cube_reduction(void) {
    enum { slot=1640 };
    unsigned checks=0;
    mtl_queue_activate(slot);
    tex_t target={.serial=0x18b10,.d={.width=2,.height=2,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA32F}};
    nvn_tex_layout(&target.d,&target.L);
    for (int array=0;array<2;array++) {
        tex_t texture={.serial=0x18b00+array,.d={.width=4,.height=4,.depth=6*(array+1),.levels=2,
            .target=array ? NVN_TEXTURE_TARGET_CUBEMAP_ARRAY : NVN_TEXTURE_TARGET_CUBEMAP,.format=NVN_FORMAT_RGBA32F}};
        nvn_tex_layout(&texture.d,&texture.L);mtl_reg_tex(slot,&texture,NULL);
        list_t upload={0};
        for (int layer=0;layer<=array;layer++) for (int mip=0;mip<2;mip++) for (int face=0;face<6;face++) {
            float value=100*layer+10*mip+face+1;
            p_clear_tex clear={.tex=&texture,.has_view=1,.view={.set_levels=1,.base_level=mip},
                .r={.zoffset=6*layer+face,.width=4>>mip,.height=4>>mip,.depth=1},.mask=15,.c.f={value,200-value,2*value,1}};
            put(&upload,OP_CLEAR_TEX,&clear,sizeof clear);
        }
        put(&upload,OP_END,NULL,0);mtl_sub_begin();mtl_exec(upload.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (int separate=0;separate<2;separate++) for (int form=0;form<4;form++) {
            int fragment=form>=2,stage=fragment ? 1 : 5;
            prog_t *program=&cube_reduction_program[array+2*separate+4*form];program->nstage=fragment ? 2 : 1;
            for (int x=-1;x<=1;x++) for (int y=-1;y<=1;y++) for (int z=-1;z<=1;z++) {
                int components=!!x+!!y+!!z;if (!components || (fragment && components!=1)) continue;
                int directions[]={x,y,z},first=6,last=0,axis=0;
                for (int d=0;d<3;d++) if (directions[d]) {
                    int face=2*d+(directions[d]<0);first=first<face ? first : face;last=last>face ? last : face;axis=d;
                }
                for (int layer=0;layer<=array;layer++) for (int level=0;level<3;level++)
                for (int nearest=0;nearest<2;nearest++) for (int mode=1;mode<=2;mode++) {
                    if (nearest && components!=1) continue; // Tied nearest faces are implementation-selected.
                    float lod=.5f*level;
                    samp_par sampler={.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,
                        .wr=NVN_WRAP_MODE_CLAMP_TO_EDGE,.minf=nearest ? NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR :
                        NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR,.magf=!nearest,.lod_max=1,.reduction=mode};
                    mtl_reg_samp(slot,&sampler);
                    float parameters[4][4]={{2*x,2*y,2*z,layer},{0},{0},{lod}},result[16];
                    if (form==1) memcpy(parameters[1],(float[]){x,y,z,0},sizeof parameters[1]);
                    // Along-face direction derivatives at radius two produce
                    // rho=2^lod after cube projection and the four-texel scale.
                    // The radial component must cancel under the quotient rule,
                    // including at edges/corners where the direction is oblique.
                    if (axis==0) parameters[1][2]-=x*exp2f(lod);
                    else parameters[1][0]+=(axis==2 ? z : 1)*exp2f(lod);
                    memset(result,0xcc,sizeof result);
                    list_t list={0};
                    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=program,.stages=fragment ? 3 : 0x20);
                    RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=stage|BIND_TEX1_NOBASE,.slot=0,.h=((uint64_t)slot<<20)|slot);
                    RECORD(OP_BIND_SAMP,p_bind_handles,.stage=stage,.first=0,.n=1,.h={(uint64_t)slot<<20});
                    RECORD(OP_BIND_UBO,p_bind_buf,.stage=stage,.index=0,.addr=(uintptr_t)parameters,.size=sizeof parameters);
                    if (fragment) {
                        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
                        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,2,2}});
                        RECORD(OP_SCISSOR,p_scissor,.w=2,.h=2);
                        RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={-100,-100,-100,-100});
                        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
                        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)result,.r={.width=2,.height=2,.depth=1});
                    } else {
                        RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)result,.size=sizeof result);
                        RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
                    }
                    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
                    float lo=100*layer+10*(level==2)+first+1,hi=100*layer+10*(level!=0)+last+1;
                    float expected[]={mode==1 ? lo : hi,200-(mode==1 ? hi : lo),2*(mode==1 ? lo : hi),1};
                    if (form==3) memcpy(expected,(float[]){lod,lod,0,1},sizeof expected);
                    for (int ch=0;ch<4;ch++) if (!isfinite(result[ch]) || fabsf(result[ch]-expected[ch])>0.0001f)
                        die("cube reduction array%d separate%d form%d dir%d/%d/%d layer%d level%d nearest%d mode%d ch%d: %g expected %g",
                            array,separate,form,x,y,z,layer,level,nearest,mode,ch,result[ch],expected[ch]);
                    checks++;
                }
            }
        }
        mtl_reg_tex(slot,NULL,NULL);mtl_tex_release(&texture);
    }
    mtl_tex_release(&target);mtl_queue_forget(slot);
    printf("Cube min/max passed %u face, edge, corner, mip, gradient, implicit/query and independent binding cases\n",checks);
}
