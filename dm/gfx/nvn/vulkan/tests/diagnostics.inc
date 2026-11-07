#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../../../third_party/flip/tool/stb_image.h"

void dm_gfx_texraw(uint64_t serial,const char *path);
void dm_gfx_texlist(int target);
void dm_gfx_texdump_next(const char *path);

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
    // Match the present pass: square VI layer fitted inside a wide output,
    // crop-edge clamping, bilinear interpolation and either raster origin.
    assert(!setenv("DM_NVN_SHOT_OUT","8x4",1));mtl_present_layer(1,1);
    for (int flipped=0;flipped<2;flipped++) {
        unlink(png);mtl_set_origin_lower_left(flipped);dm_gfx_shot_at(1,png);mtl_present(&texture,0,crop);
        int sw,sh,components;uint8_t *image=stbi_load(png,&sw,&sh,&components,4);assert(image && sw==8 && sh==4);
        for (unsigned y=0;y<4;y++) for (unsigned x=0;x<8;x++) {
            uint8_t *pixel=image+4*(y*8+x);int red=0,green=0;
            if (x>=2 && x<6) {
                float u=fminf(1,fmaxf(0,((x-2)+.5f)*.5f-.5f));
                float v=fminf(1,fmaxf(0,(y+.5f)*.5f-.5f));if (flipped) v=1-v;
                red=lroundf(40*(1+u));green=lroundf(60*(1+v));
            }
            assert(abs(pixel[0]-red)<=1 && abs(pixel[1]-green)<=1 && !pixel[2] && pixel[3]==255);
        }
        stbi_image_free(image);
    }
    unsetenv("DM_NVN_SHOT_OUT");mtl_present_layer(0,0);mtl_set_origin_lower_left(0);
    tex_t hdr={.serial=0x17d02,.d={.width=2,.height=2,.depth=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA16F}};
    nvn_tex_layout(&hdr.d,&hdr.L);list=(list_t){0};
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&hdr});RECORD(OP_SCISSOR,p_scissor,.w=2,.h=2);
    RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=15,.rgba={.25f,.5f,1,.75f});
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    unlink(png);dm_gfx_shot_at(1,png);mtl_present(&hdr,0,NULL);
    int hw,hh,hc;uint8_t *hi=stbi_load(png,&hw,&hh,&hc,4);assert(hi && hw==2 && hh==2);
    for (unsigned i=0;i<4;i++) assert(hi[4*i]==64 && hi[4*i+1]==128 && hi[4*i+2]==255 && hi[4*i+3]==255);
    stbi_image_free(hi);mtl_tex_release(&hdr);
    tex_t target={.serial=0x17d01,.d={.width=8,.height=8,.depth=2,.levels=2,
        .target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_RGBA16F}};
    nvn_tex_layout(&target.d,&target.L);
    dm_gfx_texdump_next("canceled-%d.png");
    list=(list_t){0};
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target},.has_cview={1},
        .cview={{.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=1,.nlayers=1}});
    RECORD(OP_SCISSOR,p_scissor,.x=0,.y=0,.w=4,.h=4);
    RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=15,.rgba={.25f,.5f,1.f,.75f});
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    dm_gfx_texdump_next("diagnostic-target-%d.png"); // Release the canceled request's pins.
    mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    mtl_tex_release(&target); // The pending dump owns the selected image.
    mtl_present(&texture,0,NULL);
    const char *target_path="diagnostic-target-97537-1-1.png";
    int target_w,target_h,components;uint8_t *target_image=stbi_load(target_path,&target_w,&target_h,&components,4);
    assert(target_image && target_w==4 && target_h==4);
    for (unsigned i=0;i<16;i++) {
        assert(target_image[4*i]==64 && target_image[4*i+1]==128 && target_image[4*i+2]==255 && target_image[4*i+3]==255);
    }
    stbi_image_free(target_image);unlink(target_path);
    unlink(raw);dm_gfx_texraw(UINT64_MAX,raw);assert(access(raw,F_OK));
    dm_gfx_texraw(texture.serial,"/proc/nxnative-unwritable.raw");
    dm_gfx_shot_at(1,"/proc/nxnative-unwritable.png");mtl_present(&texture,0,NULL);
    mtl_reg_tex(1650,NULL,NULL);mtl_tex_release(&texture);mtl_pool_del(pixels);mtl_queue_forget(1650);
    mtl_set_origin_lower_left(0);unlink(png);
    assert(!chdir(previous) && !rmdir(directory));free(previous);
    puts("Graphics controls preserve delayed/scaled PNG crops, both origins, float conversion, raw slices, retained mip-layer targets and failed outputs");
}
