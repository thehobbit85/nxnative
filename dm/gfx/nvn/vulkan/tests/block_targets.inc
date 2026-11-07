static void block_targets(void) {
    enum { SLOT=1648 };
    unsigned checks=0;
    mtl_queue_activate(SLOT);
    const int shapes[]={NVN_TEXTURE_TARGET_2D,NVN_TEXTURE_TARGET_2D_ARRAY,NVN_TEXTURE_TARGET_3D};
    for (unsigned format=0x41;format<=0x50;format++) for (unsigned shape=0;shape<3;shape++) for (unsigned mip=0;mip<2;mip++) {
        tex_t texture={.serial=0x17b00+(format-0x41)*6+shape*2+mip,.has_layout=1,
            .d={.width=8,.height=8,.depth=shape ? 4 : 1,.levels=2,.target=shapes[shape],.format=format}};
        nvn_tex_layout(&texture.d,&texture.L);
        texture.mem=calloc(1,texture.L.size);uint8_t *expected=calloc(1,texture.L.size);assert(texture.mem && expected);
        mtl_pool_add(texture.mem,texture.L.size);
        unsigned bytes=(format==0x41 || format==0x42 || format==0x45 || format==0x46 || format==0x49 || format==0x4a) ? 8 : 16;
        uint8_t block[16]={0};float sampled[4]={0}, want[4]={0,0,0,1};
        if (format<=0x48) {
            if (bytes==16) { memset(block,255,8);block[8]=0;block[9]=0xf8; }
            else { block[0]=0;block[1]=0xf8; }
            want[0]=1;
        } else if (format<=0x4c) {
            block[0]=(format&1) ? 255 : 127;want[0]=1;
            if (bytes==16) { block[8]=block[0];want[1]=1; }
        } else if (format<=0x4e) {
            const uint8_t mode6[]={0xc0,0xff,0x5f,0xa5,2,0,0x98,0xcc,1};memcpy(block,mode6,sizeof mode6);
            want[0]=1;want[1]=85/255.f;want[2]=1/255.f;want[3]=153/255.f;
            if (format==0x4e) for (unsigned c=0;c<3;c++) want[c]=want[c]<=.04045f ? want[c]/12.92f : powf((want[c]+.055f)/1.055f,2.4f);
        }
        uint32_t words[4];
        for (unsigned i=0;i<4;i++) {
            words[i]=0;memcpy(&words[i],block+i*(bytes/4),bytes/4);
        }
        uint8_t replacement[16]={63};
        int rebound=format==0x49 && shape==0 && mip==0;
        unsigned width=8>>mip, columns=(width+3)/4, layer=shape ? 1 : 0;
        uint8_t linear[4*4*16]={0};
        memcpy(linear+(shape==2 ? columns*columns*bytes*layer : 0),block,bytes);
        if (rebound) memcpy(linear+bytes,replacement,bytes);
        assert(!nvn_swizzle(&texture.d,mip,shape==1 ? layer : 0,linear,columns*bytes,expected));
        view_t view={.set_levels=1,.base_level=mip,.nlevels=1,.set_layers=1,.min_layer=layer,.nlayers=1,
            .set_format=1,.format=bytes==8 ? NVN_FORMAT_RGBA16UI : NVN_FORMAT_RGBA32UI};
        view_t sample=view;sample.set_format=0;sample.set_target=1;sample.target=NVN_TEXTURE_TARGET_2D;
        if (shape<2) mtl_reg_tex(SLOT,&texture,&sample);
        mtl_pool_add(words,sizeof words);mtl_pool_add(sampled,sizeof sampled);
        list_t list={0};
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&texture},.has_cview={1},.cview={view});
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,columns,columns}});
        RECORD(OP_SCISSOR,p_scissor,.x=0,.y=0,.w=1,.h=1);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&block_target_program,.stages=3);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)words,.size=sizeof words);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        if (rebound) {
            RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&texture,.src=(uintptr_t)replacement,
                .r={.xoffset=4,.width=4,.height=4,.depth=1});
            RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=1,.rgba={0,0,0,0});
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        }
        if (shape<2) {
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&block_sample_program,.stages=0x20);
            RECORD(OP_BIND_TEX1,p_bind_tex1,.stage=5|BIND_TEX1_NOBASE,.slot=0,.h=SLOT);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)sampled,.size=sizeof sampled);
            RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        }
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        if (memcmp(texture.mem,expected,texture.L.size)) die("BC raw target %#x shape%u mip%u encoded bytes changed outside the selected block",format,shape,mip);
        if (shape<2) for (unsigned c=0;c<4;c++) if (!(fabsf(sampled[c]-want[c])<.003f))
            die("BC raw target %#x shape%u mip%u channel%u sampled %g expected %g",format,shape,mip,c,sampled[c],want[c]);
        mtl_reg_tex(SLOT,NULL,NULL);mtl_tex_release(&texture);
        mtl_pool_del(texture.mem);mtl_pool_del(words);mtl_pool_del(sampled);
        free(texture.mem);free(expected);checks++;
    }
    mtl_queue_forget(SLOT);
    printf("Compressed target shadows preserve encoded blocks, mip/layer/3D guards and immediate samples in %u cases\n",checks);
}

static void depth_color_targets(void) {
    enum { SLOT=1649 };
    mtl_queue_activate(SLOT);
    for (unsigned wide=0;wide<2;wide++) for (unsigned array=0;array<2;array++) for (unsigned mip=0;mip<2;mip++) {
        unsigned layers=array ? 2 : 1, selected=array ? 1 : 0;
        tex_t depth={.serial=0x17c00+wide*4+array*2+mip,.d={.width=8,.height=8,.depth=layers,.levels=2,
            .target=array ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D,
            .format=wide ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_DEPTH16}};
        nvn_tex_layout(&depth.d,&depth.L);
        uint32_t words[4]={wide ? 0x3e800000 : 12345,0,0,0};
        uint8_t returned[2][8*8*2*4]={{0}};
        mtl_pool_add(words,sizeof words);mtl_pool_add(returned,sizeof returned);
        list_t list={0};
        for (unsigned level=0;level<2;level++) {
            RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&depth,.has_view=1,
                .view={.set_levels=1,.base_level=level,.nlevels=1},
                .r={.width=8>>level,.height=8>>level,.depth=layers},.mask=15,.c.f={.75f});
        }
        view_t view={.set_levels=1,.base_level=mip,.nlevels=1,.set_layers=1,.min_layer=selected,.nlayers=1,
            .set_format=1,.format=wide ? NVN_FORMAT_R32UI : NVN_FORMAT_R16UI};
        RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&depth},.has_cview={1},.cview={view});
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,8>>mip,8>>mip}});
        RECORD(OP_SCISSOR,p_scissor,.x=1,.y=1,.w=1,.h=1);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&block_target_program,.stages=3);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)words,.size=sizeof words);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        for (unsigned level=0;level<2;level++) {
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&depth,.src=(uintptr_t)returned[level],.has_view=1,
                .view={.set_levels=1,.base_level=level,.nlevels=1},.r={.width=8>>level,.height=8>>level,.depth=layers});
        }
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for (unsigned level=0;level<2;level++) for (unsigned layer=0;layer<layers;layer++) {
            unsigned size=8>>level, bytes=wide ? 4 : 2;
            for (unsigned y=0;y<size;y++) for (unsigned x=0;x<size;x++) {
                uint32_t actual=0;memcpy(&actual,returned[level]+((layer*size+y)*size+x)*bytes,bytes);
                uint32_t expected=level==mip && layer==selected && x==1 && y==1 ? words[0] : wide ? 0x3f400000 : 49151;
                if (actual!=expected) die("depth color target wide%u array%u mip%u returned level%u layer%u xy%u,%u %#x expected %#x",
                    wide,array,mip,level,layer,x,y,actual,expected);
            }
        }
        mtl_tex_release(&depth);mtl_pool_del(words);mtl_pool_del(returned);
    }
    mtl_queue_forget(SLOT);
    puts("D16 and D32F color targets preserve prior GPU depth, selected pixels, mip and layer guards");
}
