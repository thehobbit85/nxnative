#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../../../third_party/flip/tool/stb_image.h"

void dm_gfx_texraw(uint64_t serial,const char *path);
void dm_gfx_texlist(int target);

static void graphics_diagnostics(void) {
#ifdef __ANDROID__
    char directory[]="/data/local/tmp/nx-diagnostics-XXXXXX";
#else
    char directory[]="/tmp/nx-diagnostics-XXXXXX";
#endif
    char *previous=getcwd(NULL,0);assert(previous && mkdtemp(directory) && !chdir(directory));
    const char *png="diagnostic-shot.png", *raw="diagnostic-texture.raw";
    unlink(png);unlink(raw);
    tex_t texture={.serial=0x17d00,.d={.width=4,.height=4,.depth=2,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&texture.d,&texture.L);
    uint8_t pixels[2][4][4][4];
    for (unsigned layer=0;layer<2;layer++) for (unsigned y=0;y<4;y++) for (unsigned x=0;x<4;x++) {
        pixels[layer][y][x][0]=40*x;pixels[layer][y][x][1]=60*y;
        pixels[layer][y][x][2]=70*layer;pixels[layer][y][x][3]=7;
    }
    mtl_queue_activate(1650);mtl_pool_add(pixels,sizeof pixels);
    list_t list={0};
    RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&texture,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=2});
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    mtl_reg_tex(1650,&texture,NULL);dm_gfx_texlist(NVN_TEXTURE_TARGET_2D_ARRAY);
    dm_gfx_texraw(texture.serial,raw);
    FILE *file=fopen(raw,"rb");assert(file);
    unsigned w,h,format,row,slices;
    assert(fscanf(file,"%u %u %u %u %u",&w,&h,&format,&row,&slices)==5 && fgetc(file)=='\n');
    assert(w==4 && h==4 && format==VK_FORMAT_R8G8B8A8_UNORM && row==16 && slices==2);
    uint8_t actual[sizeof pixels];assert(fread(actual,1,sizeof actual,file)==sizeof actual && fgetc(file)==EOF);
    fclose(file);assert(!memcmp(actual,pixels,sizeof actual));
    int crop[]={1,1,2,2};
    for (int flipped=0;flipped<2;flipped++) {
        unlink(png);mtl_set_origin_lower_left(flipped);
        dm_gfx_shot_at(2,png);
        mtl_present(&texture,0,crop);assert(access(png,F_OK));
        mtl_present(&texture,0,crop);
        int components;uint8_t *image=stbi_load(png,(int *)&w,(int *)&h,&components,4);
        assert(image && w==2 && h==2);
        for (unsigned y=0;y<2;y++) for (unsigned x=0;x<2;x++) {
            uint8_t *p=image+4*(y*2+x);
            assert(p[0]==40*(x+1) && p[1]==60*(flipped ? 2-y : y+1) && !p[2] && p[3]==255);
        }
        stbi_image_free(image);
    }
    unlink(raw);dm_gfx_texraw(UINT64_MAX,raw);assert(access(raw,F_OK));
    dm_gfx_texraw(texture.serial,"/proc/nxnative-unwritable.raw");
    dm_gfx_shot_at(1,"/proc/nxnative-unwritable.png");mtl_present(&texture,0,NULL);
    mtl_reg_tex(1650,NULL,NULL);mtl_tex_release(&texture);mtl_pool_del(pixels);mtl_queue_forget(1650);
    mtl_set_origin_lower_left(0);unlink(png);
    assert(!chdir(previous) && !rmdir(directory));free(previous);
    puts("Graphics controls preserve delayed PNG crops/origin, raw array slices and failed-output diagnostics");
}
