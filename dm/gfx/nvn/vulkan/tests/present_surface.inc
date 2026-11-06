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

// Return WSI failures before ownership changes. For SUBOPTIMAL the real call
// still hands the caller an image or queues it for display.
static VkResult next_acquire=VK_SUCCESS, next_present=VK_SUCCESS, next_caps=VK_SUCCESS;
static unsigned swapchains;
static int incomplete_images;
static VkSemaphore *image_ready;
static uint32_t image_count;
VkResult __real_vkGetSwapchainImagesKHR(VkDevice,VkSwapchainKHR,uint32_t *,VkImage *);
VkResult __wrap_vkGetSwapchainImagesKHR(VkDevice dev,VkSwapchainKHR sc,uint32_t *count,VkImage *images) {
    VkResult real=__real_vkGetSwapchainImagesKHR(dev,sc,count,images);
    if (images && incomplete_images && real==VK_SUCCESS) { incomplete_images=0; return VK_INCOMPLETE; }
    return real;
}
VkResult __real_vkAcquireNextImageKHR(VkDevice,VkSwapchainKHR,uint64_t,VkSemaphore,VkFence,uint32_t *);
VkResult __wrap_vkAcquireNextImageKHR(VkDevice device,VkSwapchainKHR chain,uint64_t timeout,
                                      VkSemaphore sem,VkFence fence,uint32_t *index) {
    VkResult injected=next_acquire; next_acquire=VK_SUCCESS;
    if (injected!=VK_SUCCESS && injected!=VK_SUBOPTIMAL_KHR) return injected;
    VkResult real=__real_vkAcquireNextImageKHR(device,chain,timeout,sem,fence,index);
    return real==VK_SUCCESS ? injected : real;
}
VkResult __real_vkQueuePresentKHR(VkQueue,const VkPresentInfoKHR *);
VkResult __wrap_vkQueuePresentKHR(VkQueue q,const VkPresentInfoKHR *p) {
    assert(p->swapchainCount==1 && p->waitSemaphoreCount==1 && p->pImageIndices[0]<image_count);
    uint32_t index=p->pImageIndices[0];
    for (uint32_t i=0;i<image_count;i++) if (i!=index) assert(image_ready[i]!=p->pWaitSemaphores[0]);
    if (image_ready[index]) assert(image_ready[index]==p->pWaitSemaphores[0]);
    image_ready[index]=p->pWaitSemaphores[0];
    VkResult injected=next_present; next_present=VK_SUCCESS;
    if (injected!=VK_SUCCESS && injected!=VK_SUBOPTIMAL_KHR) return injected;
    VkResult real=__real_vkQueuePresentKHR(q,p);
    return real==VK_SUCCESS ? injected : real;
}
VkResult __real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice,VkSurfaceKHR,VkSurfaceCapabilitiesKHR *);
VkResult __wrap_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd,VkSurfaceKHR s,VkSurfaceCapabilitiesKHR *out) {
    VkResult injected=next_caps; next_caps=VK_SUCCESS;
    if (injected!=VK_SUCCESS) return injected;
    return __real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd,s,out);
}
VkResult __real_vkCreateSwapchainKHR(VkDevice,const VkSwapchainCreateInfoKHR *,const VkAllocationCallbacks *,VkSwapchainKHR *);
VkResult __wrap_vkCreateSwapchainKHR(VkDevice dev,const VkSwapchainCreateInfoKHR *ci,const VkAllocationCallbacks *a,VkSwapchainKHR *out) {
    VkResult real=__real_vkCreateSwapchainKHR(dev,ci,a,out);
    if (real==VK_SUCCESS) {
        swapchains++;
        assert(__real_vkGetSwapchainImagesKHR(dev,*out,&image_count,NULL)==VK_SUCCESS && image_count);
        free(image_ready); image_ready=calloc(image_count,sizeof *image_ready); assert(image_ready);
    }
    return real;
}

static void surface_ticket(const tex_t *texture,uint64_t expected,int dropped,const int crop[4]) {
    assert(mtl_present(texture,0,crop)==expected);
    uint64_t queued,done;
    mtl_present_progress(&queued,&done);
    assert(queued==expected && done==expected);
    assert(mtl_presented_time(expected)==(dropped ? -1.0 : 0.0));
}
static void *first_surface_ticket(void *t) { surface_ticket(t,1,1,NULL); return NULL; }

static const uint8_t window_colors[4][4]={{251,13,37,255},{16,196,65,255},{41,77,188,255},{207,148,91,255}};

// Expected linear interpolation for a square VI layer and an 8x8 quadrant
// image. Clamp to the requested rectangle before interpolating at its edges.
static void surface_pixels(AImageReader *reader,int width,int height,const int crop[4],int flipped,int wide_layer) {
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
    int rw=width,rh=height;
    const char *fit=getenv("DM_SCREEN_FIT");
    if (!fit || strcmp(fit,"stretch")) {
        int aspect=wide_layer ? 2 : 1;
        if (width>height*aspect) rw=height*aspect; else rh=width/aspect;
    }
    int x0=(width-rw)/2,y0=(height-rh)/2;
    int cx=crop && crop[2] ? crop[0] : 0, cy=crop && crop[3] ? crop[1] : 0;
    int cw=crop && crop[2] ? crop[2] : 8, ch=crop && crop[3] ? crop[3] : 8;
    for (int y=0;y<h;y++) for (int x=0;x<w;x++) {
        const uint8_t *got=bytes+y*row+x*pixel;
        if (x<x0 || x>=x0+rw || y<y0 || y>=y0+rh) {
            assert(!memcmp(got,(uint8_t[]){0,0,0,255},4)); continue;
        }
        double u=(x-x0+.5)/rw, v=(y-y0+.5)/rh;
        double sx=fmin(cx+cw-1,fmax(cx,cx+u*cw-.5));
        double sy=fmin(cy+ch-1,fmax(cy,cy+(flipped ? 1-v : v)*ch-.5));
        int ix=(int)sx,iy=(int)sy;
        double fx=sx-ix,fy=sy-iy;
        for (int c=0;c<4;c++) {
            double want=0;
            for (int dy=0;dy<2;dy++) for (int dx=0;dx<2;dx++)
                want+=window_colors[(ix+dx>=4)+2*(iy+dy>=4)][c]*(dx?fx:1-fx)*(dy?fy:1-fy);
            if (fabs(got[c]-want)>1.1) die("present crop %d,%d %dx%d flipped %d at %d,%d channel %d: %u expected %.1f",
                                         cx,cy,cw,ch,flipped,x,y,c,got[c],want);
        }
    }
    AImage_delete(image);
}

static void surface_failures(AImageReader *reader,tex_t *texture,int width,int height,uint64_t *ticket) {
    struct { VkResult acquire,present; int dropped,recreate; } cases[]={
        {VK_TIMEOUT,VK_SUCCESS,1,0},{VK_NOT_READY,VK_SUCCESS,1,0},
        {VK_SUBOPTIMAL_KHR,VK_SUCCESS,0,1},{VK_SUCCESS,VK_SUBOPTIMAL_KHR,0,1},
        {VK_ERROR_OUT_OF_DATE_KHR,VK_SUCCESS,1,1},{VK_SUCCESS,VK_ERROR_OUT_OF_DATE_KHR,1,1},
        {VK_ERROR_SURFACE_LOST_KHR,VK_SUCCESS,1,1},{VK_SUCCESS,VK_ERROR_SURFACE_LOST_KHR,1,1}
    };
    for (size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        unsigned before=swapchains;
        next_acquire=cases[i].acquire; next_present=cases[i].present;
        surface_ticket(texture,(*ticket)++,cases[i].dropped,NULL);
        if (!cases[i].dropped) surface_pixels(reader,width,height,NULL,0,0);
        surface_ticket(texture,(*ticket)++,0,NULL);
        surface_pixels(reader,width,height,NULL,0,0);
        assert(swapchains==before+cases[i].recreate);
    }
}

static void surface_history(void) {
    alarm(30);
    mtl_init();
    static const int formats[] = {NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8_SRGB, NVN_FORMAT_BGRA8_SRGB};
    tex_t textures[3] = {0};
    list_t list={0};
    uint8_t input[3][8][8][4];
    for (int i=0;i<3;i++) {
        textures[i]=(tex_t){.serial=0xe817+i,.d={.target=NVN_TEXTURE_TARGET_2D,.format=formats[i],
                            .width=8,.height=8,.depth=1,.levels=1}};
        nvn_tex_layout(&textures[i].d,&textures[i].L);
        for (int y=0;y<8;y++) for (int x=0;x<8;x++) {
            const uint8_t *color=window_colors[(x>=4)+2*(y>=4)];
            for (int c=0;c<4;c++) input[i][y][x][c]=color[i==2 && (c==0 || c==2) ? 2-c : c];
        }
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&textures[i],.src=(uintptr_t)input[i],.r={.width=8,.height=8,.depth=1});
    }
    mtl_present_layer(1,1);
    put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    pthread_t producer; assert(!pthread_create(&producer,NULL,first_surface_ticket,textures));
    pthread_mutex_lock(&fixture_lock);
    while (!entered) pthread_cond_wait(&fixture_ready,&fixture_lock);
    uint64_t queued,done; mtl_present_progress(&queued,&done); assert(queued==1 && done==0);
    hold_first=0; pthread_cond_broadcast(&fixture_ready); pthread_mutex_unlock(&fixture_lock);
    assert(!pthread_join(producer,NULL));
    assert(!mtl_has_window());
    uint64_t ticket=2;
    const int sizes[][2]={{32,32},{40,24},{24,40}};
    for (int pass=0;pass<3;pass++) {
        int width=sizes[pass][0],height=sizes[pass][1];
        AImageReader *reader=NULL;
        assert(AImageReader_new(width,height,AIMAGE_FORMAT_RGBA_8888,4,&reader)==AMEDIA_OK);
        assert(AImageReader_getWindow(reader,&surface_window)==AMEDIA_OK && surface_window);
        surface_generation++;
        assert(mtl_has_window());
        if (pass) {
            next_caps=VK_ERROR_SURFACE_LOST_KHR;
            surface_ticket(textures,ticket++,1,NULL);
            incomplete_images=1;
            surface_ticket(textures,ticket++,1,NULL);
            assert(!incomplete_images);
        }
        const int crops[][4]={{7,5,0,0},{4,0,4,4},{0,4,4,4},{3,3,1,1},{1,1,4,4}};
        for (int flip=0;flip<2;flip++) {
            mtl_set_origin_lower_left(flip);
            for (size_t i=0;i<sizeof crops/sizeof *crops;i++) {
                for (int f=0;f<3;f++) {
                    surface_ticket(&textures[f],ticket++,0,crops[i]);
                    surface_pixels(reader,width,height,crops[i],flip,0);
                }
            }
        }
        mtl_set_origin_lower_left(0);
        mtl_present_layer(2,1);
        mtl_present_screen(16,9);  // Source and requested screen differ: use the VI layer.
        surface_ticket(textures,ticket++,0,crops[1]);
        surface_pixels(reader,width,height,crops[1],0,1);
        mtl_present_screen(1,1);   // Matching source aspect takes precedence over the VI layer.
        surface_ticket(textures,ticket++,0,crops[1]);
        surface_pixels(reader,width,height,crops[1],0,0);
        mtl_present_layer(1,1); mtl_present_screen(0,0);
        surface_failures(reader,textures,width,height,&ticket);
        const uint64_t last_valid=ticket-1;
        const int bad[][4]={{0,0,0,1},{-1,0,2,2},{7,6,2,2},{1,1,INT32_MAX,INT32_MAX}};
        for (size_t i=0;i<sizeof bad/sizeof *bad;i++) surface_ticket(textures,ticket++,1,bad[i]);
        surface_window=NULL; surface_generation++;
        assert(!mtl_has_window());
        surface_ticket(textures,ticket++,1,NULL);
        assert(mtl_presented_time(last_valid)==0);
        AImageReader_delete(reader);
    }
    for (int i=0;i<3;i++) mtl_tex_release(&textures[i]);
    free(image_ready);
    alarm(0);
    puts("VI tickets, crop pixels, acquire/present failure recovery and real resized scanout passed");
}
