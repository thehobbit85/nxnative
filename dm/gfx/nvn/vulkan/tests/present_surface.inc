// An ImageReader consumes real Vulkan presents, including after a lost window.
#include <android/native_window.h>
#include <media/NdkImageReader.h>

static ANativeWindow *surface_window;
static int surface_generation;
static pthread_mutex_t fixture_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fixture_ready=PTHREAD_COND_INITIALIZER;
static int hold_first=1,entered;
void *front_native_window(int *generation) { *generation=surface_generation; return surface_window; }
void front_window_lock(int on) {
    if (!on) return;
    pthread_mutex_lock(&fixture_lock);
    entered=1; pthread_cond_broadcast(&fixture_ready);
    while (hold_first) pthread_cond_wait(&fixture_ready,&fixture_lock);
    pthread_mutex_unlock(&fixture_lock);
}

static void surface_ticket(const tex_t *texture,uint64_t expected,int dropped) {
    assert(mtl_present(texture,0,NULL)==expected);
    uint64_t queued,done;
    mtl_present_progress(&queued,&done);
    assert(queued==expected && done==expected);
    assert(mtl_presented_time(expected)==(dropped ? -1.0 : 0.0));
}
static void *first_surface_ticket(void *t) { surface_ticket(t,1,1); return NULL; }

static void surface_pixels(AImageReader *reader,int width,int height) {
    AImage *image=NULL;
    uint64_t start=now_ns();
    while (AImageReader_acquireNextImage(reader,&image)==AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE &&
           now_ns()-start<UINT64_C(5000000000)) usleep(1000);
    assert(image);
    int32_t w,h,row,pixel; uint8_t *bytes=NULL; int length=0;
    assert(AImage_getWidth(image,&w)==AMEDIA_OK && AImage_getHeight(image,&h)==AMEDIA_OK);
    assert(w==width && h==height);
    assert(AImage_getPlaneRowStride(image,0,&row)==AMEDIA_OK);
    assert(AImage_getPlanePixelStride(image,0,&pixel)==AMEDIA_OK && pixel>=4);
    assert(AImage_getPlaneData(image,0,&bytes,&length)==AMEDIA_OK && length>=(h-1)*row+(w-1)*pixel+4);
    // The active picture includes the center regardless of the aspect fit.
    assert(!memcmp(bytes+(h/2)*row+(w/2)*pixel,(uint8_t[]){255,0,0,255},4));
    AImage_delete(image);
}

static void surface_history(void) {
    alarm(30);
    mtl_init();
    tex_t texture={.serial=0xe817,.d={.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,
                   .width=16,.height=16,.depth=1,.levels=1}};
    nvn_tex_layout(&texture.d,&texture.L);
    list_t list={0};
    RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&texture,.r={.width=16,.height=16,.depth=1},.mask=15,.c.f={1,0,0,1});
    put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    pthread_t producer; assert(!pthread_create(&producer,NULL,first_surface_ticket,&texture));
    pthread_mutex_lock(&fixture_lock);
    while (!entered) pthread_cond_wait(&fixture_ready,&fixture_lock);
    uint64_t queued,done; mtl_present_progress(&queued,&done); assert(queued==1 && done==0);
    hold_first=0; pthread_cond_broadcast(&fixture_ready); pthread_mutex_unlock(&fixture_lock);
    assert(!pthread_join(producer,NULL));
    uint64_t ticket=2;
    for (int pass=0;pass<2;pass++) {
        int width=pass ? 40 : 32,height=pass ? 24 : 32;
        AImageReader *reader=NULL;
        assert(AImageReader_new(width,height,AIMAGE_FORMAT_RGBA_8888,4,&reader)==AMEDIA_OK);
        assert(AImageReader_getWindow(reader,&surface_window)==AMEDIA_OK && surface_window);
        surface_generation++;
        surface_ticket(&texture,ticket++,0);
        surface_pixels(reader,width,height);
        surface_window=NULL; surface_generation++;
        surface_ticket(&texture,ticket++,1);
        assert(mtl_presented_time(ticket-2)==0);
        AImageReader_delete(reader);
    }
    mtl_tex_release(&texture);
    alarm(0);
    puts("VI tickets and displayed pixels passed across surface creation, loss and resized recreation");
}
