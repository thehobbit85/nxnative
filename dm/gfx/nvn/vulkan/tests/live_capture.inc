#include "../capture_format.h"
#include "../../../../android/compat/CommonCrypto/CommonDigest.h"

static void live_capture(void) {
    const char *path=getenv("DM_VK_TEST_CAPTURE_PATH");
    assert(path && *path);
    char selection[1200];snprintf(selection,sizeof selection,"1:1:%s",path);
    uint8_t storage[12288]={0};
    pool_t pool={.mem=storage,.size=sizeof storage};
    tex_t texture={.magic=MAGIC('T','E','X',0),.serial=0x17e00,.pool=&pool,.mem=storage+4096,.off=4096,
        .d={.width=16,.height=16,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&texture.d,&texture.L);
    tex_t output=texture;output.serial++;output.mem=storage+8192;output.off=8192;
    mtl_queue_activate(1700);mtl_pool_add(storage,sizeof storage);
    list_t list={0};
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&texture});
    RECORD(OP_SCISSOR,p_scissor,.w=16,.h=16);
    RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=15,.rgba={1,0,0,1});
    put(&list,OP_END,NULL,0);
    mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    list=(list_t){0};
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
    RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=15,.rgba={0,1,0,1});
    put(&list,OP_END,NULL,0);
    mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    mtl_reg_tex(1700,&texture,NULL);
    mtl_reg_samp(1700,&(samp_par){.magf=NVN_MAG_FILTER_NEAREST,.minf=NVN_MIN_FILTER_NEAREST});
    assert(!setenv("DM_NVN_CAPTURE",selection,1));
    for (int submit=0;submit<2;submit++) {
        storage[0]=(uint8_t)(17+submit);
        list=(list_t){0};
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&program);
        if (!submit) {
            RECORD(OP_COPY_T2T,p_copy_t2t,.src=&texture,.dst=&output,
                .sr={.width=16,.height=16,.depth=1},.dr={.width=16,.height=16,.depth=1});
        } else {
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
            RECORD(OP_SCISSOR,p_scissor,.w=8,.h=8);
            RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=15,.rgba={0,0,1,1});
        }
        put(&list,OP_END,NULL,0);
        list_t outer={0};uint64_t child=(uintptr_t)list.bytes;
        put(&outer,OP_CALL,&child,sizeof child);put(&outer,OP_END,NULL,0);
        uint64_t handle=(uintptr_t)outer.bytes;
        nvn_cap_submit(1,&handle);
        mtl_sub_begin();mtl_exec(outer.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    }
    nvn_cap_present(&output,NULL);
    cap_reader reader;assert(cap_load(&reader,path));
    cap_chunk chunk;uint8_t *data;
    int next,submits=0,segments=0,regions=0,diffs=0,images=0,programs=0,success=0,presents=0;
    uint8_t expected[16*16*4],digest[32];
    for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) {
        unsigned i=(y*16+x)*4;int blue=x<8 && y<8;
        expected[i]=blue ? 0 : 255;expected[i+1]=0;expected[i+2]=blue ? 255 : 0;expected[i+3]=255;
    }
    CC_SHA256(expected,sizeof expected,digest);
    while ((next=cap_next(&reader,&chunk,&data))>0) {
        switch (chunk.tag) {
            case CAP_HEAD: assert(memmem(data,chunk.raw,"backend\tvulkan\n",15));break;
            case CAP_LAYT: assert(cap_layout_matches((char *)data,chunk.raw));break;
            case CAP_REGN: {
                cap_regn *region=(cap_regn *)data;
                assert(region->size==sizeof storage && chunk.raw==sizeof *region+sizeof storage);
                assert(data[sizeof *region]==17);regions++;break;
            }
            case CAP_PDIF: {
                uint64_t offset,length;memcpy(&offset,data+8,8);memcpy(&length,data+16,8);
                assert(!offset && length==4096 && data[24]==18);diffs++;break;
            }
            case CAP_PROG: {
                cap_prog *p=(cap_prog *)data;assert(chunk.raw==sizeof *p && p->ok==3 && p->iface[0] && p->iface[1]);programs++;break;
            }
            case CAP_TPIX: {
                cap_tpix *image=(cap_tpix *)data;assert(image->pixfmt==VK_FORMAT_R8G8B8A8_UNORM && image->nsub==1);
                cap_tsub *sub=(cap_tsub *)(image+1);assert(sub->bytes==sizeof expected);
                uint8_t *pixels=(uint8_t *)(sub+1);
                assert(image->t.serial==texture.serial || image->t.serial==output.serial);
                int red=image->t.serial==texture.serial;
                for (unsigned i=0;i<sizeof expected;i+=4) assert(pixels[i]==(red ? 255 : 0) && pixels[i+1]==(red ? 0 : 255) && !pixels[i+2] && pixels[i+3]==255);
                images++;break;
            }
            case CAP_SEGM: {
                uint32_t length;memcpy(&length,data+4,4);assert(length+8==chunk.raw);
                op_hdr first;memcpy(&first,data+8,sizeof first);
                if (first.op==OP_CALL) { uint64_t target;memcpy(&target,data+16,8);assert(cap_obj_kind(target)==CAP_K_SEG && cap_obj_index(target)==1); }
                segments++;break;
            }
            case CAP_SUBM: { uint32_t count;memcpy(&count,data,4);assert(count==1);submits++;break; }
            case CAP_PRES: {
                cap_pres *present=(cap_pres *)data;assert(present->bytes==sizeof expected && !memcmp(present->sha256,digest,32));presents++;break;
            }
            case CAP_LPIX: assert(chunk.raw==sizeof expected && !memcmp(data,expected,sizeof expected));break;
            case CAP_VK_RESULT: memcpy(&success,data,sizeof success);break;
        }
        free(data);
    }
    assert(!next && reader.pos==reader.n && success==1 && submits==2 && segments==4 && regions==1 && diffs==1 && images==2 && programs==1 && presents==1);
    cap_unload(&reader);
    mtl_reg_tex(1700,NULL,NULL);mtl_tex_release(&texture);mtl_tex_release(&output);mtl_pool_del(storage);mtl_queue_forget(1700);
    puts("Live Vulkan capture preserves nested commands, two submits, pool diffs, shader interfaces, GPU inputs and the presented digest");
}
