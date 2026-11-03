// MediaCodec accepts Annex B access units and returns pictures in presentation order,
// including B pictures held until EOS. Copy them before releasing their codec buffer.
typedef struct avc_frame {
    struct avc_frame *next;
    int w, h, standard, full;
    int64_t pts;
    uint8_t nv12[];
} avc_frame;
static void close_session(mavc *a) {
    if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); a->mc = NULL; }
    a->eos_in = a->eos_out = a->fed_picture = 0;
}
void mavc_drop(mavc *a) {
    close_session(a); // start the next stream with fresh CSD even when flushed before the first output
    while (a->first) { avc_frame *next = a->first->next; free(a->first); a->first = next; }
    a->last = NULL; a->nq = 0;
}
void mavc_free(mavc *a) { if (a) { mavc_drop(a); free(a->au); free(a); } }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }
static int open_session(mavc *a) {
    if (!a->coded_w || !a->coded_h || !a->npps) return 0;
    AMediaFormat *f = AMediaFormat_new();
    if (!f) return 0;
    uint8_t c0[516] = {0,0,0,1}, c1[516] = {0,0,0,1};
    memcpy(c0 + 4, a->sps, a->nsps); memcpy(c1 + 4, a->pps, a->npps);
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, a->coded_w);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, a->coded_h);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 19); // OMX_COLOR_FormatYUV420Planar
    AMediaFormat_setBuffer(f, "csd-0", c0, a->nsps + 4); AMediaFormat_setBuffer(f, "csd-1", c1, a->npps + 4);
    a->mc = AMediaCodec_createDecoderByType("video/avc");
    int ok = a->mc && AMediaCodec_configure(a->mc, f, NULL, NULL, 0) == AMEDIA_OK && AMediaCodec_start(a->mc) == AMEDIA_OK;
    AMediaFormat_delete(f);
    if (!ok) { close_session(a); return 0; }
    a->dirty = 0; return 1;
}
static int format_int(AMediaFormat *f, const char *key, int fallback) {
    int32_t v; return AMediaFormat_getInt32(f, key, &v) ? v : fallback;
}
static int copy_picture(mavc *a, AMediaFormat *f, const uint8_t *src, size_t bytes, int64_t pts) {
    int cw = format_int(f, "width", 0), ch = format_int(f, "height", 0);
    int stride = format_int(f, "stride", cw), slice = format_int(f, "slice-height", ch);
    if (slice == 0) slice = ch;  // an unspecified slice height uses the picture's height
    int cf = format_int(f, "color-format", 0);
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
    *fr = (avc_frame){.w=w,.h=h,.pts=pts,.standard=format_int(f,"color-standard",h >= 720 ? 1 : 4),.full=format_int(f,"color-range",2)==1};
    for (int y = 0; y < h; y++) memcpy(fr->nv12 + (size_t)y*w, src + (size_t)(t+y)*stride + l, w);
    uint8_t *uv = fr->nv12 + (size_t)w*h;
    for (int y = 0; y < uvh; y++) for (int x = 0; x < uvw; x++) {
        size_t at = (size_t)y*cs + x*step;
        *uv++ = src[cbase+at]; *uv++ = src[vbase+at];
    }
    if (a->last) a->last->next = fr; else a->first = fr;
    a->last = fr; a->nq++; return 1;
}
// One output, format change or EOS. The negative result signals a codec/layout failure.
static int drain(mavc *a, int64_t wait_us) {
    AMediaCodecBufferInfo bi;
    ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, wait_us);
    if (i == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return 0;
    if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return 1;
    if (i < 0) return -1;
    int ok = 1;
    if (bi.size > 0 && !(bi.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
        size_t cap = 0; uint8_t *src = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &cap);
        AMediaFormat *f = AMediaCodec_getBufferFormat(a->mc, (size_t)i);
        ok = src && f && bi.offset >= 0 && (size_t)bi.offset <= cap && (size_t)bi.size <= cap-bi.offset &&
             copy_picture(a, f, src+bi.offset, bi.size, bi.presentationTimeUs);
        if (f) AMediaFormat_delete(f);
    }
    if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) a->eos_out = 1;
    if (AMediaCodec_releaseOutputBuffer(a->mc, i, false) != AMEDIA_OK) ok = 0;
    return ok ? 1 : -1;
}
// The guest may be holding its movie lock while waiting for a decode. Fail a nonresponsive
// codec instead of leaving the worker and every thread waiting on that lock parked forever.
static int64_t codec_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int feed(mavc *a, const uint8_t *p, size_t n, int64_t pts, uint32_t flags) {
    int64_t deadline = codec_now_ms() + 5000;
    do {
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 10000);
        if (i >= 0) {
            size_t cap = 0; uint8_t *dst = AMediaCodec_getInputBuffer(a->mc, i, &cap);
            if (n && (!dst || n > cap)) return 0;
            if (n) memcpy(dst, p, n);
            return AMediaCodec_queueInputBuffer(a->mc, i, 0, n, pts, flags) == AMEDIA_OK;
        }
        if (i != AMEDIACODEC_INFO_TRY_AGAIN_LATER || drain(a, 0) < 0) return 0;
    } while (codec_now_ms() < deadline);
    return 0;
}
static int finish(mavc *a, int all) {
    if (!a->eos_in) {
        if (!feed(a, NULL, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) return 0;
        a->eos_in = 1;
    }
    int64_t deadline = codec_now_ms() + 5000;
    while (!a->eos_out && (all || !a->first)) {
        int r = drain(a, 10000);
        if (r < 0 || (!r && codec_now_ms() >= deadline)) return 0;
        if (r) deadline = codec_now_ms() + 5000;
    }
    return 1;
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
    if (a->mc && (a->dirty || a->eos_in || restart)) {
        if (!finish(a, 1)) return codec_error(a);
        close_session(a);
    }
    if (!a->mc && !open_session(a)) return codec_error(a);
    if (!feed(a, a->au, c.len, pts, 0)) return codec_error(a);
    if (!a->fed_picture || pts > a->max_pts) a->max_pts = pts;
    a->fed_picture = 1;
    return 0;
}
static uint8_t color8(int x) { return x < 0 ? 0 : x > 255 ? 255 : (uint8_t)x; }
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts) {
    if (!a->first && a->mc) {
        int ok = 1;
        if (flush) ok = finish(a, 0);
        else while (!a->first && !a->eos_out) { int r = drain(a, 0); if (r <= 0) { ok = r == 0; break; } }
        if (!ok) codec_error(a);
    }
    avc_frame *f = a->first;
    if (!f) return 0;
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
        memset(dst, 16, (size_t)w*h); memset(dst+(size_t)w*h,128,(size_t)w*h/2);
        for (int y=0; y<ch; y++) memcpy(dst+(size_t)y*w,f->nv12+(size_t)y*f->w,cw);
        for (int y=0; y<(ch+1)/2 && y<h/2; y++) memcpy(dst+(size_t)w*h+(size_t)y*w,uv+(size_t)y*cs,((cw+1)&~1) > w ? w : ((cw+1)&~1));
    }
    if (pts) *pts=f->pts;
    a->first=f->next; if (!a->first) a->last=NULL; a->nq--; free(f); return 1;
}
