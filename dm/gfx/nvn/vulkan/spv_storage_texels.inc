// Storage image and texel descriptors need the format declared by the shader.
// Read the module itself so previously exported shaders work too.
typedef struct { VkFormat format; uint8_t bytes; } storage_texel_format;
static storage_texel_format spirv_texel_format(uint32_t code) {
#define F(name,size) {VK_FORMAT_##name,size}
    // SPIR-V Image Format enumerants, in registry order (0 is Unknown).
    static const storage_texel_format formats[]={
        {0,0},F(R32G32B32A32_SFLOAT,16),F(R16G16B16A16_SFLOAT,8),F(R32_SFLOAT,4),
        F(R8G8B8A8_UNORM,4),F(R8G8B8A8_SNORM,4),F(R32G32_SFLOAT,8),F(R16G16_SFLOAT,4),
        F(B10G11R11_UFLOAT_PACK32,4),F(R16_SFLOAT,2),F(R16G16B16A16_UNORM,8),F(A2B10G10R10_UNORM_PACK32,4),
        F(R16G16_UNORM,4),F(R8G8_UNORM,2),F(R16_UNORM,2),F(R8_UNORM,1),
        F(R16G16B16A16_SNORM,8),F(R16G16_SNORM,4),F(R8G8_SNORM,2),F(R16_SNORM,2),F(R8_SNORM,1),
        F(R32G32B32A32_SINT,16),F(R16G16B16A16_SINT,8),F(R8G8B8A8_SINT,4),F(R32_SINT,4),
        F(R32G32_SINT,8),F(R16G16_SINT,4),F(R8G8_SINT,2),F(R16_SINT,2),F(R8_SINT,1),
        F(R32G32B32A32_UINT,16),F(R16G16B16A16_UINT,8),F(R8G8B8A8_UINT,4),F(R32_UINT,4),
        F(A2B10G10R10_UINT_PACK32,4),F(R32G32_UINT,8),F(R16G16_UINT,4),F(R8G8_UINT,2),F(R16_UINT,2),
        F(R8_UINT,1),F(R64_UINT,8),F(R64_SINT,8)
    };
#undef F
    return code<sizeof formats/sizeof *formats ? formats[code] : (storage_texel_format){0};
}
static void reflect_storage_texels(const uint32_t *w, size_t count, sbind *bindings, int nb) {
    int has_storage=0;
    for (int i=0;i<nb;i++) has_storage |= bindings[i].type == BT_STB || bindings[i].type == BT_SIMG;
    if (!has_storage || count<5 || !w[3] || w[3]>0x3fffff) return;
    typedef struct { unsigned binding; uint32_t image; int view, num; } storage_id;
    uint32_t bound=w[3];
    storage_id *ids=calloc(bound,sizeof *ids);
    if (!ids) return;
    for (size_t i=5;i<count;) {
        const uint32_t *p=w+i; unsigned len=p[0]>>16, op=p[0]&0xffff;
        if (!len || len>count-i) break;
        if (op==71 && len==4 && p[1]<bound && p[2]==33) { // OpDecorate Binding
            for (int b=0;b<nb;b++) if ((bindings[b].type==BT_STB || bindings[b].type==BT_SIMG) && bindings[b].binding==p[3]) ids[p[1]].binding=b+1;
        } else if (op==21 && len==4 && p[1]<bound) { // OpTypeInt signedness
            ids[p[1]].num = p[3] ? 1 : 2;
        } else if (op==25 && len>=9 && p[1]<bound && p[2]<bound && p[7]==2 && p[8]<UINT32_MAX) {
            ids[p[1]].image=p[8]+1; // OpTypeImage, storage; plus one distinguishes Unknown
            ids[p[1]].num = ids[p[2]].num;
            static const int dimensions[][2] = {{VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY},
                {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY}, {VK_IMAGE_VIEW_TYPE_3D, -1},
                {VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY}};
            ids[p[1]].view = p[3] < 4 && p[5] < 2 ? dimensions[p[3]][p[5]] : -1;
        } else if ((op==28 || op==29) && len>=3 && p[1]<bound && p[2]<bound) { // array
            ids[p[1]]=ids[p[2]];
        } else if (op==32 && len==4 && p[1]<bound && p[3]<bound) {
            ids[p[1]]=ids[p[3]]; // pointer
        } else if (op==59 && len>=4 && p[1]<bound && p[2]<bound && ids[p[2]].binding) {
            sbind *b = &bindings[ids[p[2]].binding-1];
            b->storage_image=ids[p[1]].image;
            if (b->type == BT_SIMG && b->storage_image) { b->view = ids[p[1]].view; b->num = ids[p[1]].num; }
        } else if ((op==61 || op==65 || op==66 || op==83) && len>=4 && p[2]<bound && p[3]<bound) {
            ids[p[2]].binding=ids[p[3]].binding; // load, access chain, copy
        } else if (op==60 && len==6 && p[3]<bound && ids[p[3]].binding) {
            bindings[ids[p[3]].binding-1].texel_atomic=1; // OpImageTexelPointer
        }
        i+=len;
    }
    free(ids);
}
