// Keep a real guest AudioOut playing while the synthetic game presents and
// the Android framework repeatedly removes and restores its native window.
typedef struct { void *pcm; uint64_t capacity, size; } life_buffer;
extern unsigned audio_open(void *, const void *), audio_start(void *), audio_append(void *, life_buffer *);
extern life_buffer *audio_release(void *);
static uint64_t life_output[64];
static int16_t life_pcm[8][480*2] __attribute__((aligned(4096)));
static life_buffer life_buffers[8];

static void lifecycle_audio_start(void) {
    struct { uint32_t rate; uint16_t channels, pad; } config = {48000,2,0};
    require(!audio_open(life_output,&config));
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 480; j++) {
            int16_t v = j % 48 < 24 ? 700+i*7 : -700-i*7;
            life_pcm[i][2*j] = v; life_pcm[i][2*j+1] = -v;
        }
        life_buffers[i] = (life_buffer){life_pcm[i],sizeof life_pcm[i],sizeof life_pcm[i]};
        require(audio_append(life_output,&life_buffers[i]));
    }
    require(!audio_start(life_output));
}

static void lifecycle_audio_poll(void) {
    static unsigned returned;
    life_buffer *b;
    while ((b = audio_release(life_output))) {
        require(b == &life_buffers[returned % 8]);
        require(audio_append(life_output,b));
        if (++returned % 32 == 0) NOTE("APP_FIXTURE_AUDIO_PROGRESS\n");
    }
}
