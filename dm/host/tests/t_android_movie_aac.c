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

// Optional generated AAC-LC fixture: 33 unprotected ADTS frames at 44.1 or
// 48 kHz. Pass the demuxed packets to the guest API and hold all 32 outputs.
static void adts_playback(FILE *input, FILE *capture, int supplied) {
    enum { count = MD_NOUT_AAC+1 };
    void *dec[2];
    int ch = 0, rate = 0;
    double signal = 0, error = 0;
    for (int i = 0; i <= count; i++) {
        uint8_t header[7];
        size_t n = 0;
        if (i < count) {
            assert(fread(header,1,sizeof header,input) == sizeof header);
            assert(header[0] == 0xff && (header[1]&0xf7) == 0xf1 && (header[2]>>6) == 1 && !(header[6]&3));
            unsigned freq = (header[2]>>2)&15;
            int channels = ((header[2]&1)<<2) | header[3]>>6;
            assert((freq == 3 || freq == 4) && channels >= 1 && channels <= 7);
            int config_id = channels;
            if (channels == 7) channels = 8;
            if (!i) {
                rate = freq == 3 ? 48000 : 44100; ch = channels;
                uint8_t asc[] = {(2u<<3) | (freq>>1), (freq<<7) | (config_id<<3)};
                metadata config = supplied ? (metadata){.csd={{asc,sizeof asc,sizeof asc}}} : (metadata){.ch=ch,.rate=rate};
                h_movie_Decoder(dec,MD_TYPE_AAC,MD_FMT_PCM16,1,0);
                assert(!h_movie_Configure(dec,&config) && !h_movie_RegEv(dec,event_ptrs) && !h_movie_Start(dec));
            }
            assert(channels == ch && rate == (freq == 3 ? 48000 : 44100));
            unsigned len = ((header[3]&3)<<11) | (header[4]<<3) | (header[5]>>5);
            assert(len > sizeof header); n = len - sizeof header;
        }
        int idx,left; buffer b;
        assert(!h_movie_AcqIn(dec,&idx,&left) && !h_movie_GetIn(dec,idx,&b));
        assert(n <= b.cap && fread(b.p,1,n,input) == n);
        assert(!h_movie_SendIn(dec,idx,0,(int)n,(int64_t)i*FRAME*1000000/rate,i == count ? 4 : 0));
    }
    assert(fgetc(input) == EOF && !ferror(input));
    int held[MD_NOUT_AAC], nh = 0;
    for (int i = 0; i <= count; i++) {
        int idx,left; int64_t pts; uint32_t flags;
        if (nh == MD_NOUT_AAC) {
            assert(h_movie_AcqOut(dec,&idx,&pts,&flags,&left) == MD_NO_OUTPUT);
            assert(!h_movie_RelOut(dec,held[--nh]));
        }
        assert(!h_movie_AcqOut(dec,&idx,&pts,&flags,&left));
        assert(llabs(pts - (int64_t)i*FRAME*1000000/rate) <= 1 && flags == (i == count ? 4 : 0));
        int16_t samples[FRAME*8];
        buffer b = {(uint8_t *)samples, sizeof samples, 0}; metadata fmt = {0};
        assert(!h_movie_GetOutFmt(dec,idx,&b,&fmt) && fmt.ch == ch && fmt.rate == rate);
        assert(b.size == (i == count ? 0 : FRAME*ch*sizeof(int16_t)));
        if (supplied) {
            int16_t reference[FRAME*8];
            assert(fread(reference,1,b.size,capture) == b.size);
            for (size_t j = 0; j < b.size/sizeof(int16_t); j++) {
                double v = reference[j], delta = samples[j]-v;
                signal += v*v; error += delta*delta;
            }
        } else assert(fwrite(samples,1,b.size,capture) == b.size);
        held[nh++] = idx;
    }
    while (nh) assert(!h_movie_RelOut(dec,held[--nh]));
    if (supplied) assert(signal > 0 && error*3162 < signal); // at least 35 dB for explicit vs inferred ASC
    assert(!h_movie_Stop(dec)); h_movie_DecD1(dec);
}

int main(int argc, char **argv) {
    assert(argc <= 3);
    FILE *capture = argc >= 2 ? fopen(argv[1], "w+b") : NULL;
    assert(argc < 2 || capture);
    G = (__typeof__(G)){.signal=sdk_signal, .set_data=set_data, .set_range=set_range, .data=data, .cap=cap,
        .size=size, .set_i32=set_i32, .find_i32=find_i32, .find_buf=find_buf};
    if (argc == 3) {
        FILE *input = fopen(argv[2],"rb"); assert(input);
        adts_playback(input,capture,0);
        rewind(input); rewind(capture);
        adts_playback(input,capture,1);
        assert(!fclose(input) && !fclose(capture));
        puts("AAC-LC guest PCM and 32-slot timestamped drain passed");
        return 0;
    }
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
