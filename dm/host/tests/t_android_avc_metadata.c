// The returned frame's declared matrix/range override resolution heuristics,
// and queued frames must not overwrite the metadata of the last returned one.
#include "../hle_movie_avc.c"
#include <assert.h>

static int rgb_byte(double value) {
    return value <= 0 ? 0 : value >= 255 ? 255 : (int)(value + 0.5);
}

// Visible 3x3 within padded 6x6 I420/NV12 input; guest asks for a 6x4 picture.
static void cropped_picture(int layout, int standard, int full, int rgba) {
    enum { stride = 8, slice = 8, ysize = stride * slice, w = 6, h = 4 };
    uint8_t src[96], out[4*w*h + 1];
    memset(src, 0xee, sizeof src); memset(out, 0xbb, sizeof out);
    const uint8_t yplane[9] = {46, 75, 100, 82, 110, 135, 155, 135, 112};
    const uint8_t uplane[4] = {88, 160, 85, 140}, vplane[4] = {96, 148, 165, 115};
    for (int y = 0; y < 3; y++) memcpy(src + (2+y)*stride + 2, yplane + 3*y, 3);
    int cs = layout == 19 ? stride/2 : stride, step = layout == 19 ? 1 : 2;
    int u = ysize + cs + step, v = u + (layout == 19 ? (slice/2)*cs : 1);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {
        src[u + y*cs + x*step] = uplane[y*2+x];
        src[v + y*cs + x*step] = vplane[y*2+x];
    }
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setInt32(fmt,"width",6); AMediaFormat_setInt32(fmt,"height",6);
    AMediaFormat_setInt32(fmt,"stride",stride); AMediaFormat_setInt32(fmt,"slice-height",slice);
    AMediaFormat_setInt32(fmt,"color-format",layout); AMediaFormat_setInt32(fmt,"color-standard",standard);
    AMediaFormat_setInt32(fmt,"color-range",full ? 1 : 2); AMediaFormat_setRect(fmt,"crop",2,2,4,4);
    mavc *a = mavc_new();
    if (rgba) mavc_set_rgba(a);
    assert(!copy_picture(a,fmt,src,v+cs+step,42) && !mavc_queued(a));
    assert(copy_picture(a,fmt,src,v+cs+step+1,42));
    int64_t pts = 0;
    assert(mavc_take(a,0,out,w,h,&pts) && pts == 42 && !mavc_queued(a));
    assert(out[rgba ? sizeof out-1 : w*h*3/2] == 0xbb);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int inside = x < 3 && y < 3;
        int luma = inside ? yplane[3*y+x] : (full ? 0 : 16);
        if (!rgba) { assert(out[y*w+x] == luma); continue; }
        int c = 2*(y/2) + x/2;
        double cb = inside ? uplane[c]-128 : 0, cr = inside ? vplane[c]-128 : 0;
        double yy = full ? luma : (luma-16)*255./219.;
        if (!full) { cb *= 255./224.; cr *= 255./224.; }
        double kr = standard == 1 ? 0.2126 : standard == 6 ? 0.2627 : 0.299;
        double kb = standard == 1 ? 0.0722 : standard == 6 ? 0.0593 : 0.114;
        double r = yy + 2*(1-kr)*cr, b = yy + 2*(1-kb)*cb;
        double g = (yy - kr*r - kb*b)/(1-kr-kb);
        const uint8_t *p = &out[4*(y*w+x)];
        assert(abs(p[0]-rgb_byte(r)) <= 1 && abs(p[1]-rgb_byte(g)) <= 1 && abs(p[2]-rgb_byte(b)) <= 1 && p[3] == 255);
    }
    if (!rgba) for (int y = 0; y < h/2; y++) for (int x = 0; x < w; x += 2) {
        int i = y*2+x/2, j = w*h+y*w+x;
        assert(out[j] == (x < 4 ? uplane[i] : 128) && out[j+1] == (x < 4 ? vplane[i] : 128));
    }
    AMediaFormat_delete(fmt); mavc_free(a);
}
static void unspecified_hd_matrix(void) {
    enum { w = 2, h = 720 };
    uint8_t src[w*h*3/2], out[w*h*4];
    memset(src, 128, w*h);
    for (int i = w*h; i < sizeof src; i += 2) { src[i] = 88; src[i+1] = 96; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setInt32(f,"width",w); AMediaFormat_setInt32(f,"height",h);
    AMediaFormat_setInt32(f,"color-format",21);
    AMediaFormat_setInt32(f,"color-standard",0); // explicitly unspecified
    mavc *a = mavc_new(); mavc_set_rgba(a);
    assert(copy_picture(a,f,src,sizeof src,0) && mavc_take(a,0,out,w,h,NULL));
    assert(mavc_bt709(a,h) && !mavc_fullrange(a));
    // Limited-range BT.709 output for Y=128, Cb=88, Cr=96.
    assert(abs(out[0]-73) <= 1 && abs(out[1]-156) <= 1 && abs(out[2]-46) <= 1);
    AMediaFormat_delete(f); mavc_free(a);
}

int main(void) {
    unspecified_hd_matrix();
    mavc *a = mavc_new();
    assert(!mavc_bt709(a, 480) && mavc_bt709(a, 720));
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setInt32(format, "width", 2);
    AMediaFormat_setInt32(format, "height", 2);
    AMediaFormat_setInt32(format, "color-format", 21);
    uint8_t picture[] = {16, 16, 16, 16, 128, 128}, out[6];
    for (int i = 0; i < 2; i++) {
        AMediaFormat_setInt32(format, "color-standard", i ? 4 : 1);
        AMediaFormat_setInt32(format, "color-range", i ? 1 : 2);
        assert(copy_picture(a, format, picture, sizeof picture, i));
    }
    assert(mavc_take(a, 0, out, 2, 2, NULL));
    assert(mavc_bt709(a, 480) && !mavc_fullrange(a));
    assert(mavc_take(a, 0, out, 2, 2, NULL));
    assert(!mavc_bt709(a, 720) && mavc_fullrange(a));
    mavc_drop(a);
    assert(mavc_bt709(a, 720) && !mavc_fullrange(a));
    AMediaFormat_delete(format);
    mavc_free(a);
    const int standards[] = {1,4,6};
    for (int layout = 19; layout <= 21; layout += 2) for (int s = 0; s < 3; s++)
        for (int full = 0; full <= 1; full++) for (int rgba = 0; rgba <= 1; rgba++)
            cropped_picture(layout, standards[s], full, rgba);
    puts("AVC frame metadata, cropped/padded I420 and NV12, and BT.601/709/2020 RGB ranges passed");
}
