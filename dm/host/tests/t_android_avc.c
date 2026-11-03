// Six synthetic access units with B pictures. Check all planes, presentation order, restart and black padding.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hle_movie_avc.h"
#include "h264_synthetic.h"
static void stream(mavc *a, int rgba, int reset_pts) {
    enum { width = 68, height = 66, guard = 16 };
    uint8_t frame[width * height * 4 + 2 * guard];
    int n = 0;
    for (int i = 0; i <= 12; i++) {
        if (i < 12) {
            unsigned j = i % 6;
            int64_t pts = packets[j].pts + (reset_pts || i < 6 ? 0 : 240000);
            if (rgba) {
                // Each synthetic access unit contains one slice. Replace its length with an Annex B start code.
                uint8_t au[256] = {0,0,0,1}; assert(packets[j].n <= sizeof au);
                memcpy(au + 4, packets[j].p + 4, packets[j].n - 4);
                assert(!mavc_decode(a, au, (int)packets[j].n, pts));
            } else assert(!mavc_decode(a, packets[j].p, (int)packets[j].n, pts));
        }
        int64_t pts;
        memset(frame, 0x37, sizeof frame);
        uint8_t *dst = frame + guard;
        while (mavc_take(a, i == 12, dst, width, height, &pts)) {
            assert(n < 12);
            int idx = n % 6;
            assert(pts == (reset_pts ? idx : n) * 40000);
            size_t bytes = rgba ? width * height * 4 : width * height * 3 / 2;
            for (int k = 0; k < guard; k++) assert(frame[k] == 0x37 && dst[bytes + k] == 0x37);
            for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
                int inside = x < 64 && y < 64;
                if (!rgba) assert(abs(dst[y*width+x] - (inside ? yuv[idx][0] : 16)) <= 4);
                else {
                    const uint8_t *pixel = dst + 4 * (y * width + x);
                    static const uint8_t rgb[6][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,255,255},{255,0,255}};
                    for (int c = 0; c < 3; c++) assert(abs(pixel[c] - (inside ? rgb[idx][c] : 0)) <= 8);
                    assert(pixel[3] == 255);
                }
            }
            if (!rgba) for (int y = 0; y < height / 2; y++) for (int x = 0; x < width; x++)
                assert(abs(dst[width * height + y*width+x] - (x < 64 && y < 32 ? yuv[idx][1+(x&1)] : 128)) <= 6);
            n++;
        }
    }
    assert(n == 12);
}
int main(void) {
    for (int rgba = 0; rgba <= 1; rgba++) {
        mavc *a = mavc_new();
        if (rgba) mavc_set_rgba(a);
        if (rgba) {
            size_t n = (size_t)avcc[6] << 8 | avcc[7], at = 9 + n;
            assert(at + 2 <= sizeof avcc);
            mavc_param_sets(a, avcc + 8, n);
            n = (size_t)avcc[at] << 8 | avcc[at+1]; assert(n <= sizeof avcc - at - 2);
            mavc_param_sets(a, avcc + at + 2, n);
        } else mavc_avcc(a, avcc, sizeof avcc);
        stream(a, rgba, 1);
        mavc_drop(a);
        stream(a, rgba, 0);
        mavc_free(a);
    }
    puts("H.264 B frames, overlapping sequence timestamps, EOS, seek, padded NV12 and RGBA passed");
}
