// Public SDK Vorbis buffers on Apple and Android. Alternate short/long windows,
// hold every output slot, drain by returning one slot, and repeat after a seek.
#include "../hle_movie.c"
#include <assert.h>
#include "movie_guest_fixture.h"
#include "movie_audio_fixture.h"
#include "vorbis_synthetic.h"

enum { frames = MD_NOUT + 2 };
static int window(int i) { return i % 5 < 3 ? 256 : 64; }
static int64_t timestamp(int samples, int64_t start) { return start + samples*1000000ll/48000; }

static void submit(void **dec, int packet, int64_t pts) {
    int idx, left; buffer b;
    assert(!h_movie_AcqIn(dec,&idx,&left) && !h_movie_GetIn(dec,idx,&b));
    vpkt p = {0};
    if (packet >= 0) vaudio(&p,2,window(packet) == 256,packet && window(packet-1) == 256,window(packet+1) == 256);
    int n = (p.bits+7)/8;
    memcpy(b.p,p.data,n);
    assert(!h_movie_SendIn(dec,idx,0,n,pts,packet < 0 ? 4 : 0));
}

static void playback(void **dec, int64_t start, FILE *capture) {
    assert(!h_movie_Start(dec));
    int offsets[frames+1] = {0};
    submit(dec,0,start); // first packet primes the overlap
    for (int i = 0; i < frames; i++) {
        offsets[i+1] = offsets[i] + (window(i) + window(i+1))/4;
        submit(dec,i+1,timestamp(offsets[i],start));
    }
    submit(dec,-1,timestamp(offsets[frames],start));
    int held[MD_NOUT], count = 0, nonzero = 0;
    for (int i = 0; i <= frames; i++) {
        int idx,left; int64_t pts; uint32_t flags;
        if (count == MD_NOUT) {
            assert(h_movie_AcqOut(dec,&idx,&pts,&flags,&left) == MD_NO_OUTPUT);
            assert(!h_movie_RelOut(dec,held[--count]));
        }
        assert(!h_movie_AcqOut(dec,&idx,&pts,&flags,&left));
        assert(pts == timestamp(offsets[i],start) && flags == (i == frames ? 4 : 0));
        struct { uint64_t pre; int16_t samples[128]; uint64_t post; } out = {.pre=7,.post=19};
        buffer b = {(uint8_t *)out.samples,sizeof out.samples,0}; metadata m = {0};
        assert(!h_movie_GetOutFmt(dec,idx,&b,&m));
        assert(out.pre == 7 && out.post == 19 && m.ch == 1 && m.rate == 48000);
        size_t n = i == frames ? 0 : offsets[i+1]-offsets[i];
        assert(b.size == n*sizeof(int16_t));
        for (size_t j = 0; j < n; j++) nonzero += out.samples[j] != 0;
        if (capture) assert(fwrite(out.samples,sizeof(int16_t),n,capture) == n);
        held[count++] = idx;
    }
    assert(nonzero > 0);
    while (count) assert(!h_movie_RelOut(dec,held[--count]));
}

int main(int argc, char **argv) {
    assert(argc <= 2);
    FILE *capture = argc == 2 ? fopen(argv[1],"wb") : NULL;
    assert(argc < 2 || capture);
    G = (__typeof__(G)){.signal=sdk_signal,.set_data=set_data,.set_range=set_range,.data=data,.cap=cap,
        .size=size,.set_i32=set_i32,.find_i32=find_i32,.find_buf=find_buf};
    vpkt id = {0}, setup = {0}; vheaders(&id,&setup,0x86,2);
    size_t idlen = (id.bits+7)/8, setlen = (setup.bits+7)/8;
    metadata config = {.ch=1,.rate=48000,.csd={{id.data,idlen,idlen},{setup.data,setlen,setlen}}};
    void *dec[2];
    h_movie_Decoder(dec,MD_TYPE_VORBIS,MD_FMT_PCM16,1,0);
    assert(!h_movie_Configure(dec,&config) && !h_movie_RegEv(dec,event_ptrs));
    playback(dec,3212345,capture);
    assert(!h_movie_Flush(dec) && !h_movie_Start(dec));
    for (int i = 0; i < 5; i++) submit(dec,i,1000*i);
    assert(!h_movie_Flush(dec));
    playback(dec,0,capture);
    assert(!h_movie_Stop(dec));
    h_movie_DecD1(dec);
    assert(!dec[1] && events[0] && events[1] && !events[3]);
    if (capture) assert(!fclose(capture));
    puts("Vorbis PCM, 64 held outputs, window changes, timestamped EOS and seek passed");
}
