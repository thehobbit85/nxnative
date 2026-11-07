// Use the same coordinates, texel pattern and analytic border filter as the
// Metal t_border fixture. Native custom-border support can be disabled here.
static void sampler_border(void) {
    enum { slot=1691 };
    const float coordinates[][2]={{-.1f,.5f},{.5f,1.05f},{1.2f,1.2f},{.06f,.94f},{.5f,.5f},
        {0,0},{1,.3f},{.97f,-.02f},{.3f,.6f}};
    const float borders[][4]={{.5f,.5f,.5f,.5f},{.25f,.5f,.75f,1},{.8f,.9f,.7f,.6f},
        {-.25f,1.25f,.1f,.3f},{0,0,0,1}};
    tex_t source={.serial=0x19000,.d={.width=4,.height=4,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA32F}};
    tex_t target={.serial=0x19001,.d={.width=1,.height=1,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA32F}};
    nvn_tex_layout(&source.d,&source.L);nvn_tex_layout(&target.d,&target.L);
    mtl_queue_activate(slot);mtl_reg_tex(slot,&source,NULL);
    float pixels[4][4][4];
    for (int y=0;y<4;y++) for (int x=0;x<4;x++)
        memcpy(pixels[y][x],(float[]){x/4.0f,y/4.0f,(x+y)/8.0f,1-x/8.0f},16);
    list_t upload={0};p_copy_b2t copy={.tex=&source,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1}};
    put(&upload,OP_COPY_B2T,&copy,sizeof copy);put(&upload,OP_END,NULL,0);
    mtl_sub_begin();mtl_exec(upload.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    unsigned checks=0;
    for (int form=0;form<5;form++) for (int separate=0;separate<2;separate++) for (unsigned border=0;border<sizeof borders/sizeof borders[0];border++) for (int linear=0;linear<2;linear++) {
        samp_par sampler={.ws=NVN_WRAP_MODE_CLAMP_TO_BORDER,.wt=NVN_WRAP_MODE_CLAMP_TO_BORDER,
            .minf=linear,.magf=linear};memcpy(sampler.border,borders[border],16);mtl_reg_samp(slot,&sampler);
        for (unsigned sample=0;sample<sizeof coordinates/sizeof coordinates[0];sample++) {
            float x=coordinates[sample][0]*4+(form==2),y=coordinates[sample][1]*4-(form==2),result[4],expected[4]={0};
            if (linear) { x-=.5f;y-=.5f; }
            int x0=(int)floorf(x),y0=(int)floorf(y);float fx=linear ? x-x0 : 0,fy=linear ? y-y0 : 0;
            for (int j=0;j<=linear;j++) for (int i=0;i<=linear;i++) {
                int xi=x0+i,yj=y0+j;
                const float *value=xi<0 || xi>=4 || yj<0 || yj>=4 ? borders[border] : pixels[yj][xi];
                float weight=(i ? fx : 1-fx)*(j ? fy : 1-fy);
                for (int ch=0;ch<4;ch++) expected[ch]+=value[ch]*weight;
            }
            int fragment=form==3,stage=fragment ? 1 : 5;
            prog_t *program=&sampler_border_program[form*2+separate];program->nstage=fragment ? 2 : 1;
            float parameters[4]={coordinates[sample][0],coordinates[sample][1]};
            list_t list={0};
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=program,.stages=fragment ? 3 : 0x20);
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=stage|BIND_TEX1_NOBASE,.slot=0,.h=((uint64_t)slot<<20)|slot);
            RECORD(OP_BIND_SAMP,p_bind_handles,.stage=stage,.first=0,.n=1,.h={(uint64_t)slot<<20});
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=stage,.index=0,.addr=(uintptr_t)parameters,.size=sizeof parameters);
            if (fragment) {
                RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
                RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,1,1}});
                RECORD(OP_SCISSOR,p_scissor,.w=1,.h=1);
                RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
                RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)result,.r={.width=1,.height=1,.depth=1});
            } else {
                RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)result,.size=sizeof result);
                RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
            }
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (int ch=0;ch<4;ch++) if (!isfinite(result[ch]) || fabsf(result[ch]-expected[ch])>.006f)
                die("Guest border form%d separate%d border%u linear%d sample%u channel%d: %g expected %g",form,separate,border,linear,sample,ch,result[ch],expected[ch]);
            checks++;
        }
    }
    mtl_reg_tex(slot,NULL,NULL);mtl_tex_release(&source);mtl_tex_release(&target);mtl_queue_forget(slot);
    printf("Guest custom border colors passed %u nearest, filtered edge/corner and interior cases\n",checks);
}
