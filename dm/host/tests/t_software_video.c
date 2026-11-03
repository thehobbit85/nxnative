// Same 4:2:0 packet contents on native and bionic builds of the bundled codec libraries.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hle_movie_av1.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include "video_packets.h"

static void plane(const uint8_t *p, size_t stride, int width, int height, int step, int want) {
    for (int r = 0; r < height; r++)
        for (int x = 0; x < width; x++)
            assert(abs((int)p[r * stride + x * step] - want) <= 6);
}
static void vpx_packet(vpx_codec_iface_t *iface, const uint8_t *p, size_t n) {
    vpx_codec_ctx_t ctx;
    assert(!vpx_codec_dec_init(&ctx, iface, NULL, 0));
    assert(!vpx_codec_decode(&ctx, p, n, NULL, 0));
    vpx_codec_iter_t it = NULL;
    vpx_image_t *im = vpx_codec_get_frame(&ctx, &it);
    assert(im && im->d_w == 16 && im->d_h == 16);
    plane(im->planes[0], im->stride[0], 16, 16, 1, 81);
    plane(im->planes[1], im->stride[1], 8, 8, 1, 90);
    plane(im->planes[2], im->stride[2], 8, 8, 1, 240);
    assert(!vpx_codec_destroy(&ctx));
}
int main(void) {
    vpx_packet(vpx_codec_vp8_dx(), vp8, sizeof vp8);
    vpx_packet(vpx_codec_vp9_dx(), vp9, sizeof vp9);
    setenv("DM_AV1_DAV1D", "1", 1);
    mav1 *a = mav1_new(); assert(!strcmp(mav1_backend(a), "dav1d"));
    for (int i = 0; i < 2; i++) {
        mav1_picture p;
        const uint8_t delim[] = {0x12, 0x00};
        assert(!mav1_decode(a, delim, sizeof delim, &p));
        assert(mav1_decode(a, av1, sizeof av1, &p) == 1);
        assert(p.w == 64 && p.h == 64 && p.matrix == 1 && !p.full_range);
        plane(p.y, p.ystride, 64, 64, 1, 81);
        plane(p.uv, p.uvstride, 32, 32, 2, 90);
        plane(p.uv + 1, p.uvstride, 32, 32, 2, 240);
        mav1_reset(a);
    }
    mav1_free(a);
    puts("VP8, VP9 and AV1 all planes and AV1 reset passed");
}
