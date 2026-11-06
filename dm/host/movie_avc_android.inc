// MediaCodec accepts Annex B access units and returns pictures in presentation order,
// including B pictures held until EOS. Copy them before releasing their codec buffer.
typedef struct avc_frame {
    struct avc_frame *next;
    int w, h, standard, full;
    int64_t pts;
    uint8_t nv12[];
} avc_frame;
int mavc_bt709(const mavc *a, int h) { return a->output_standard ? a->output_standard == 1 : h >= 720; }
int mavc_fullrange(const mavc *a) { return a->output_full; }
static void close_session(mavc *a) {
    media_close(&a->stream); a->fed_picture = 0;
}
void mavc_drop(mavc *a) {
    close_session(a); // start the next stream with fresh CSD even when flushed before the first output
    while (a->first) { avc_frame *next = a->first->next; free(a->first); a->first = next; }
    a->last = NULL; a->nq = 0; a->output_standard = a->output_full = 0;
}
void mavc_free(mavc *a) { if (a) { mavc_drop(a); free(a->au); free(a); } }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }
static int avc_ready(const void *);
static int copy_picture(void *, AMediaFormat *, const uint8_t *, size_t, int64_t);
static int avc_start(AMediaCodec *codec, AMediaFormat *f) {
    return codec && AMediaCodec_configure(codec, f, NULL, NULL, 0) == AMEDIA_OK && AMediaCodec_start(codec) == AMEDIA_OK;
}
static AMediaCodec *avc_decoder(AMediaFormat *f, int full_range) {
    AMediaCodec *codec = AMediaCodec_createDecoderByType("video/avc");
    char *name = NULL;
    if (full_range && codec && AMediaCodec_getName(codec, &name) == AMEDIA_OK) {
        int bridge = !strcmp(name, "c2.goldfish.h264.decoder");
        AMediaCodec_releaseName(codec, name);
        // Goldfish converts full-range samples to limited while still labelling them full.
        // Its AOSP software decoder preserves both. Keep the default for limited-range
        // streams and other drivers, and fall back if the codec cannot start this profile.
        if (bridge) {
            AMediaCodec *software = AMediaCodec_createCodecByName("c2.android.avc.decoder");
            if (avc_start(software, f)) { AMediaCodec_delete(codec); return software; }
            if (software) AMediaCodec_delete(software);
            LOG("[RED] movie: AOSP AVC decoder unavailable; emulator bridge may mislabel full-range output");
        }
    }
    if (avc_start(codec, f)) return codec;
    if (codec) AMediaCodec_delete(codec);
    return NULL;
}
static int open_session(mavc *a) {
    if (!a->coded.w || !a->coded.h || !a->npps) return 0;
    AMediaFormat *f = AMediaFormat_new();
    if (!f) return 0;
    uint8_t c0[516] = {0,0,0,1}, c1[516] = {0,0,0,1};
    memcpy(c0 + 4, a->sps, a->nsps); memcpy(c1 + 4, a->pps, a->npps);
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, a->coded.w);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, a->coded.h);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 19); // OMX_COLOR_FormatYUV420Planar
    AMediaFormat_setBuffer(f, "csd-0", c0, a->nsps + 4); AMediaFormat_setBuffer(f, "csd-1", c1, a->npps + 4);
    a->stream = (media_stream){.codec=avc_decoder(f, a->coded.full_range), .owner=a, .copy=copy_picture, .ready=avc_ready};
    AMediaFormat_delete(f);
    if (!a->stream.codec) { close_session(a); return 0; }
    a->dirty = 0; return 1;
}
static int avc_ready(const void *owner) { return ((const mavc *)owner)->first != NULL; }
static int copy_picture(void *owner, AMediaFormat *f, const uint8_t *src, size_t bytes, int64_t pts) {
    mavc *a = owner;
    int cw = media_format_int(f, "width", 0), ch = media_format_int(f, "height", 0);
    int stride = media_format_int(f, "stride", cw), slice = media_format_int(f, "slice-height", ch);
    if (slice == 0) slice = ch;  // an unspecified slice height uses the picture's height
    int cf = media_format_int(f, "color-format", 0);
    if (cf != 19 && cf != 21) {
        if (!a->red++) LOG("[RED] movie: MediaCodec AVC output colour format %#x has no explicit I420 or NV12 layout", cf);
        return 0;
    }
    int32_t l = 0, t = 0, r = cw-1, b = ch-1;
    AMediaFormat_getRect(f, "crop", &l, &t, &r, &b);
    if (cw <= 0 || ch <= 0 || stride < cw || slice < ch || stride > 32768 || slice > 32768 ||
        l < 0 || t < 0 || r < l || b < t || r >= cw || b >= ch || ((l | t) & 1)) return 0;
    int w = r-l+1, h = b-t+1, uvw = (w+1)/2, uvh = (h+1)/2;
    size_t ysize = (size_t)stride*slice, cs = cf == 19 ? (stride+1)/2 : stride;
    size_t cbase = ysize + (size_t)(t/2)*cs + (cf == 19 ? l/2 : l);
    size_t vbase = cf == 19 ? cbase + cs*((slice+1)/2) : cbase+1;
    size_t step = cf == 19 ? 1 : 2;
    if ((size_t)b * stride + r >= bytes || vbase + (uvh-1)*cs + (uvw-1)*step >= bytes) return 0;
    avc_frame *fr = malloc(sizeof *fr + (size_t)w*h + (size_t)uvw*2*uvh);
    if (!fr) return 0;
    int standard = media_format_int(f,"color-standard",0);
    *fr = (avc_frame){.w=w,.h=h,.pts=pts,.standard=standard ? standard : (h >= 720 ? 1 : 4),.full=media_format_int(f,"color-range",2)==1};
    for (int y = 0; y < h; y++) memcpy(fr->nv12 + (size_t)y*w, src + (size_t)(t+y)*stride + l, w);
    uint8_t *uv = fr->nv12 + (size_t)w*h;
    for (int y = 0; y < uvh; y++) for (int x = 0; x < uvw; x++) {
        size_t at = (size_t)y*cs + x*step;
        *uv++ = src[cbase+at]; *uv++ = src[vbase+at];
    }
    if (a->last) a->last->next = fr; else a->first = fr;
    a->last = fr; a->nq++; return 1;
}
static int codec_error(mavc *a) {
    if (!a->red++) LOG("[RED] movie: MediaCodec AVC configuration, input or decoded picture failed");
    close_session(a); return -1;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts) {
    au_ctx c = {a,0};
    if (!p || n <= 0) return 0;
    each_nal(p, n, au_add, &c);
    if (!c.picture) return 0;
    // A demuxer can loop a closed GOP without an EOS/flush. MediaCodec may discard the
    // previous GOP's reordered tail when the new IDR repeats its timestamps. Drain first.
    int restart = c.idr && a->fed_picture && pts <= a->max_pts;
    if (a->stream.codec && (a->dirty || a->stream.eos_in || restart)) {
        if (!media_finish(&a->stream, 1)) return codec_error(a);
        close_session(a);
    }
    if (!a->stream.codec && !open_session(a)) return codec_error(a);
    if (!media_feed(&a->stream, a->au, c.len, pts, 0)) return codec_error(a);
    if (!a->fed_picture || pts > a->max_pts) a->max_pts = pts;
    a->fed_picture = 1;
    return 0;
}
static uint8_t color8(int x) { return x < 0 ? 0 : x > 255 ? 255 : (uint8_t)x; }
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts) {
    if (!a->first && a->stream.codec) {
        int ok = 1;
        if (flush) ok = media_finish(&a->stream, 0);
        else while (!a->first && !a->stream.eos_out) { int r = media_drain(&a->stream, 0); if (r <= 0) { ok = r == 0; break; } }
        if (!ok) codec_error(a);
    }
    avc_frame *f = a->first;
    if (!f) return 0;
    a->output_standard = f->standard; a->output_full = f->full;
    int cw = f->w < w ? f->w : w, ch = f->h < h ? f->h : h, cs = (f->w + 1) & ~1;
    const uint8_t *uv = f->nv12 + (size_t)f->w*f->h;
    if (a->rgba) {
        // Android color-standard 1 = BT.709, 6 = BT.2020 and 2/4 = BT.601.
        int r = 409, gu = 100, gv = 208, b = 516;
        if (f->standard == 1) { r = 459; gu = 55; gv = 136; b = 541; }
        if (f->standard == 6) { r = 430; gu = 48; gv = 167; b = 548; }
        if (f->full) { r=r*224/255; gu=gu*224/255; gv=gv*224/255; b=b*224/255; }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++, dst += 4) {
            int yy=0,u=0,v=0;
            if (x<cw && y<ch) {
                yy=f->nv12[(size_t)y*f->w+x]; yy=f->full ? yy*256 : (yy-16)*298;
                size_t i=(size_t)(y/2)*cs+(x & ~1); u=uv[i]-128; v=uv[i+1]-128;
            }
            dst[0]=color8((yy+r*v+128)>>8); dst[1]=color8((yy-gu*u-gv*v+128)>>8); dst[2]=color8((yy+b*u+128)>>8); dst[3]=255;
        }
    } else {
        memset(dst, f->full ? 0 : 16, (size_t)w*h); memset(dst+(size_t)w*h,128,(size_t)w*h/2);
        for (int y=0; y<ch; y++) memcpy(dst+(size_t)y*w,f->nv12+(size_t)y*f->w,cw);
        for (int y=0; y<(ch+1)/2 && y<h/2; y++) memcpy(dst+(size_t)w*h+(size_t)y*w,uv+(size_t)y*cs,((cw+1)&~1) > w ? w : ((cw+1)&~1));
    }
    if (pts) *pts=f->pts;
    a->first=f->next; if (!a->first) a->last=NULL; a->nq--; free(f); return 1;
}
