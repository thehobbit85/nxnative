// Drive movie::Decoder's guest buffer/event API with synthetic AAC-LC packets. Only the
// calling SDK's buffer/metadata/event types and the host guest-thread entry are supplied here.
#include "../hle_movie.c"
#include <assert.h>
#include <math.h>
#include "aac_synthetic.h"

#include "movie_guest_fixture.h"
#include "movie_audio_fixture.h"

enum { NPACKETS = sizeof packets / sizeof packets[0], FRAME = 1024 };
static int64_t timestamp(int i) { return 1234567 + (int64_t)i * FRAME * 1000000 / 48000; }
// Ignore encoder priming and the short final block. Three whole tone periods per 144 samples.
static double energy(const int16_t *pcm, int ch, int hz) {
    double re = 0, im = 0;
    for (int i = FRAME; i < FRAME + 3072; i++) {
        double angle = 2 * M_PI * i * hz / 48000;
        re += pcm[2*i+ch] * cos(angle); im += pcm[2*i+ch] * sin(angle);
    }
    return re*re + im*im;
}
static void playback(void **self, FILE *capture) {
    assert(!h_movie_Start(self));
    for (int i = 0; i <= NPACKETS; i++) {
        int idx, left; buffer b;
        assert(!h_movie_AcqIn(self, &idx, &left)); assert(!h_movie_GetIn(self, idx, &b));
        size_t n = i < NPACKETS ? packets[i].n : 0;
        if (n) memcpy(b.p, packets[i].p, n);
        assert(!h_movie_SendIn(self, idx, 0, n, timestamp(i), i == NPACKETS ? 4 : 0));
    }
    int16_t pcm[NPACKETS * FRAME * 2];
    for (int i = 0; i <= NPACKETS; i++) {
        int idx, left; int64_t pts; uint32_t flags;
        assert(!h_movie_AcqOut(self, &idx, &pts, &flags, &left));
        assert(llabs(pts - timestamp(i)) <= 1);
        assert(flags == (i == NPACKETS ? 4 : 0));
        struct { uint64_t start; int16_t samples[FRAME*2]; uint64_t end; } out = {.start=123, .end=456};
        buffer b = {(uint8_t *)out.samples, sizeof out.samples, 0};
        metadata fmt = {0};
        assert(!h_movie_GetOutFmt(self, idx, &b, &fmt));
        assert(fmt.ch == 2 && fmt.rate == 48000 && out.start == 123 && out.end == 456);
        assert(b.size == (i == NPACKETS ? 0 : sizeof out.samples));
        if (b.size) memcpy(pcm + i * FRAME * 2, b.p, b.size);
        assert(!h_movie_RelOut(self, idx));
    }
    assert(energy(pcm,0,1000) > 1e12 && energy(pcm,1,2000) > 1e12);
    assert(energy(pcm,0,1000) > 100 * energy(pcm,0,2000));
    assert(energy(pcm,1,2000) > 100 * energy(pcm,1,1000));
    if (capture) assert(fwrite(pcm, sizeof pcm, 1, capture) == 1);
    int idx, left; int64_t pts; uint32_t flags;
    assert(h_movie_AcqOut(self, &idx, &pts, &flags, &left) == MD_NO_OUTPUT);
}
int main(int argc, char **argv) {
    assert(argc <= 2);
    FILE *capture = argc == 2 ? fopen(argv[1], "wb") : NULL;
    assert(argc < 2 || capture);
    G = (__typeof__(G)){.signal=sdk_signal, .set_data=set_data, .set_range=set_range, .data=data, .cap=cap,
        .size=size, .set_i32=set_i32, .find_i32=find_i32, .find_buf=find_buf};
    for (int supplied = 0; supplied <= 1; supplied++) {
        void *dec[2]; uint8_t asc[] = {0x11, 0x90};
        metadata config = supplied ? (metadata){.csd={{asc,sizeof asc,sizeof asc}}} : (metadata){.ch=2,.rate=48000};
        h_movie_Decoder(dec, MD_TYPE_AAC, MD_FMT_PCM16, 1, 0);
        assert(dec[1] && !h_movie_Configure(dec, &config) && !h_movie_RegEv(dec,event_ptrs));
        playback(dec,capture); assert(!h_movie_Flush(dec)); playback(dec,capture);
        assert(!h_movie_Stop(dec)); h_movie_DecD1(dec); assert(!dec[1]);
    }
    assert(events[0] && events[1] == 4 * (NPACKETS+1) && events[2] == 2 && !events[3]);
    if (capture) assert(!fclose(capture));
    puts("movie::Decoder AAC-LC: both stereo tones, timestamps, PCM bounds, EOS, events and replay passed");
}
