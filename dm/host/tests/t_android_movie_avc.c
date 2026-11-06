// Hold decoded guest slots across two coded sequences, then read them in reverse.
// Real MediaCodec decodes the test pictures; the SDK owns the following buffer and metadata objects.
#include "../hle_movie.c"
#include <assert.h>
#include "movie_guest_fixture.h"
#include "h264_synthetic.h"

typedef struct { int w, h, stride, slice, uv, color; buffer config; } video_metadata;
static void set_i32(void *md, const char *name, int value) {
    video_metadata *m = md;
    if (!strcmp(name, "width")) m->w = value;
    else if (!strcmp(name, "height")) m->h = value;
    else if (!strcmp(name, "nv12-y-stride")) m->stride = value;
    else if (!strcmp(name, "slice-height")) m->slice = value;
    else if (!strcmp(name, "nv12-uv-offset")) m->uv = value;
    else if (!strcmp(name, "nv12-colorspace")) m->color = value;
}
static void set_rect(void *unused, const char *key, int l, int t, int r, int b) {
    (void)unused; assert(!strcmp(key, "crop") && !l && !t && r == 63 && b == 63);
}
static int find_i32(const void *md, const char *name, int *out) {
    const video_metadata *m = md;
    if (!strcmp(name, "width")) *out = m->w;
    else if (!strcmp(name, "height")) *out = m->h;
    else return 0;
    return 1;
}
static int find_buffer(const void *md, const char *key, void **out) {
    if (strcmp(key, "avcc")) return 0;
    *out = (void *)&((const video_metadata *)md)->config;
    return 1;
}
int nvn_host_write_texture(const void *tex, const void *src, int w, int h, size_t row, int *tw, int *th) { abort(); }
int nvn_host_signal_sync(void *sync) { abort(); }

// FFmpeg h264_metadata rewrites only the VUI in the shared fixture's SPS.
// The first set declares limited BT.601 and the second declares full BT.709.
static const uint8_t sps[2][26] = {
    {0x67,0x64,0,0xa,0xac,0xd9,0x44,0x26,0x9a,0x83,3,3,0x20,0,0,3,0,0x20,0,0,6,0x41,0xe2,0x44,0xb2,0xc0},
    {0x67,0x64,0,0xa,0xac,0xd9,0x44,0x26,0x9b,0x80,0x80,0x80,0xa0,0,0,3,0,0x20,0,0,6,0x41,0xe2,0x44,0xb2,0xc0},
};
static int acquire(void **dec, int output, int64_t *pts, uint32_t *flags) {
    int idx, left;
    uint64_t end = now_ns() + 5000000000ull;
    for (;;) {
        int r = output ? h_movie_AcqOut(dec, &idx, pts, flags, &left) : h_movie_AcqIn(dec, &idx, &left);
        if (!r) return idx;
        assert(r == (output ? MD_NO_OUTPUT : MD_NO_INPUT) && now_ns() < end);
        usleep(1000);
    }
}
static void send(void **dec, int packet, int64_t start) {
    int idx = acquire(dec, 0, NULL, NULL);
    buffer b;
    assert(!h_movie_GetIn(dec, idx, &b));
    const size_t offset = 3;
    size_t n = 0;
    if (packet < 12) {
        int j = packet % 6;
        if (!j) {
            uint8_t nal[] = {0, 0, 0, 26};
            memcpy(b.p + offset, nal, sizeof nal); n += sizeof nal;
            memcpy(b.p + offset + n, sps[packet / 6], sizeof sps[0]); n += sizeof sps[0];
        }
        memcpy(b.p + offset + n, packets[j].p, packets[j].n);
        n += packets[j].n;
        start += packets[j].pts; // restart timestamps at the second IDR
    }
    assert(!h_movie_SendIn(dec, idx, (int)offset, (int)n, start, packet == 12 ? 4 : 0));
}
static void playback(void **dec, int64_t start) {
    assert(!h_movie_Start(dec));
    for (int i = 0; i <= 12; i++) send(dec, i, start);
    int slots[13];
    for (int i = 0; i <= 12; i++) {
        int64_t pts; uint32_t flags;
        slots[i] = acquire(dec, 1, &pts, &flags);
        assert(pts == start + (i % 6) * 40000 && flags == (i == 12 ? 4 : 0));
    }
    for (int i = 12; i >= 0; i--) {
        int idx = slots[i];
        if (i < 12) {
            struct { uint64_t pre; uint8_t pixels[64*64*3/2]; uint64_t post; } dst = {.pre=42, .post=43};
            buffer b = {dst.pixels, sizeof dst.pixels, 0};
            video_metadata meta = {0}, frame = {0};
            assert(!h_movie_BufFmt(dec, idx, &meta) && !h_movie_GetOutFmt(dec, idx, &b, &frame));
            if (meta.color != (i < 6 ? 1 : 4)) fprintf(stderr, "slot %d frame %d color %d expected %d\n", idx, i, meta.color, i < 6 ? 1 : 4);
            assert(meta.color == (i < 6 ? 1 : 4) && frame.color == meta.color);
            assert(meta.w == 64 && meta.h == 64 && meta.stride == 64 && meta.uv == 4096 && meta.slice == 64);
            assert(b.size == sizeof dst.pixels && dst.pre == 42 && dst.post == 43);
            for (unsigned k = 0; k < b.size; k++) {
                int component = k < 4096 ? 0 : 1 + (k & 1);
                if (abs((int)dst.pixels[k] - yuv[i % 6][component]) > 6)
                    fprintf(stderr, "slot %d frame %d byte %u is %u, expected %u; YUV %u %u %u\n", idx, i, k,
                            dst.pixels[k], yuv[i % 6][component], dst.pixels[0], dst.pixels[4096], dst.pixels[4097]);
                assert(abs((int)dst.pixels[k] - yuv[i % 6][component]) <= 6);
            }
        }
        assert(!h_movie_RelOut(dec, idx));
    }
}
int main(void) {
    G = (__typeof__(G)){.signal=sdk_signal,.set_data=set_data,.set_range=set_range,.data=data,.cap=cap,.size=size,
        .set_i32=set_i32,.find_i32=find_i32,.find_buf=find_buffer,.set_rect=set_rect};
    video_metadata config = {.w=64, .h=64, .config={(uint8_t *)avcc, sizeof avcc, sizeof avcc}};
    void *dec[2];
    h_movie_Decoder(dec, MD_TYPE_AVC, MD_FMT_NV12, 1, 0);
    assert(!h_movie_Configure(dec, &config) && !h_movie_RegEv(dec, event_ptrs));
    playback(dec, 1234567);
    assert(!h_movie_Flush(dec));
    playback(dec, 0);
    assert(!h_movie_Stop(dec) && !h_movie_Reset(dec));
    assert(!h_movie_Configure(dec, &config));
    playback(dec, 31337);
    assert(!h_movie_Stop(dec));
    h_movie_DecD1(dec);
    assert(!dec[1] && events[0] && events[1] && !events[3]);
    puts("MediaCodec guest slots retain presentation order, color matrix/range and planes across flush, EOS, stop and restart");
}
