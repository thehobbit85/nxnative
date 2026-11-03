// ARM64 Android device fixture. Build in this directory with the NDK's
// aarch64-linux-android30-clang -O1 -Wall -Werror -Imemory_profile t_audio_android.c ../hle_bigstack.c -laaudio -lm -o t_audio_android
// Run the binary on the device. AAudio opens real streams and drives every
// device callback. Observe the outgoing PCM before it leaves the process.
#include <aaudio/AAudio.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
enum { BLOCK = 240, CAPTURE = 24 * BLOCK };
static int16_t heard[2 * CAPTURE];
static _Atomic int max_frames;
static _Atomic int heard_frames, skip_leading_silence, reject_channels;
static AAudioStream_dataCallback render_callback;
static AAudioStream_errorCallback error_callback;
static aaudio_data_callback_result_t monitor(AAudioStream *s, void *a, void *data, int32_t frames) {
    aaudio_data_callback_result_t result = render_callback(s, a, data, frames);
    if (frames > atomic_load(&max_frames)) atomic_store(&max_frames, frames);
    int n = atomic_load(&heard_frames);
    int16_t *pcm = data;
    for (int i = 0; i < frames && n < CAPTURE; i++) {
        if (!n && atomic_load(&skip_leading_silence) && !pcm[2*i] && !pcm[2*i+1]) continue;
        heard[2*n] = pcm[2*i]; heard[2*n+1] = pcm[2*i+1]; n++;
    }
    atomic_store(&heard_frames, n);
    return result;
}
static void on_data(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *ctx) {
    render_callback = cb;
    AAudioStreamBuilder_setDataCallback(b, monitor, ctx);
}
static void on_error(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *ctx) {
    error_callback = cb; AAudioStreamBuilder_setErrorCallback(b, cb, ctx);
}
static int32_t channels(AAudioStream *s) {
    return atomic_exchange(&reject_channels, 0) ? 1 : AAudioStream_getChannelCount(s);
}
#define AAudioStreamBuilder_setDataCallback on_data
#define AAudioStreamBuilder_setErrorCallback on_error
#define AAudioStream_getChannelCount channels
#include "../hle_audio.c"
#undef AAudioStreamBuilder_setDataCallback
#undef AAudioStreamBuilder_setErrorCallback
#undef AAudioStream_getChannelCount
#define gbuf renderer_gbuf
#include "../hle_audren.c"
#undef gbuf

__thread _Atomic int dm_host_work;
uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
int dm_guest_as_contains(uintptr_t p) { (void)p; return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
// Each case opens one output, closes it (joining its threads), then starts the next case.
static const void *key; static void *value;
void objmap_put(const void *k, void *v) { assert(!key); key = k; value = v; }
void *objmap_get(const void *k) { return k == key ? value : NULL; }
void *objmap_del(const void *k) { if (k != key) return NULL; key = NULL; return value; }
static int event; static _Atomic int signals;
void hle_bind_system_event(void *e, int ac) { assert(e == &event && ac); }
void hle_signal_system_event(void *e) { assert(e == &event); atomic_fetch_add(&signals, 1); }
uint32_t hle_dup_readable_handle(uint32_t h) { abort(); }

static void wait_for(_Atomic int *value, int target) {
    uint64_t deadline = now_ns() + 5000000000ull;
    while (atomic_load(value) < target) { assert(now_ns() < deadline); usleep(1000); }
}
static void wait_backend(aout *a, int backend) {
    uint64_t deadline = now_ns() + 5000000000ull;
    for (;;) {
        pthread_mutex_lock(&g_aos_lock); int actual = a->backend; pthread_mutex_unlock(&g_aos_lock);
        if (actual == backend) return;
        assert(now_ns() < deadline); usleep(1000);
    }
}
static void test_guest(void) {
    enum { TOTAL = 2035 };
    static int16_t input[TOTAL][6];
    uint32_t out[16] = {0}, param[] = {48000, 6};
    assert(h_au_Open(out, &event, NULL, (uint8_t *)param) == RES_OK);
    assert(h_au_Ch(out) == 6 && h_au_State(out) == STATE_STOPPED);
    for (int i = 0; i < TOTAL; i++) {
        int x = i % 101 - 50;
        input[i][0] = x; input[i][1] = -x; input[i][2] = 1000; input[i][4] = 100;
    }
    const int offsets[] = {0, 17, 730, TOTAL};
    gbuf blocks[3];
    for (int i = 0; i < 3; i++) {
        size_t n = (offsets[i+1] - offsets[i]) * sizeof *input;
        h_au_SetBuf(&blocks[i], input[offsets[i]], n, n);
        assert(h_au_Append(out, &blocks[i]));
    }
    assert(!h_au_Released(out)); h_au_Start(out);
    wait_for(&heard_frames, TOTAL); wait_for(&signals, 1);
    for (int i = 0; i < TOTAL; i++) {
        int x = i % 101 - 50;
        assert(heard[2*i] == x + 777 && heard[2*i+1] == -x + 707);
    }
    for (int i = 0; i < 3; i++) assert(h_au_Released(out) == &blocks[i]);
    assert(!h_au_Released(out));

    // Stop with the device inside a long buffer. Both it and the untouched
    // next buffer must be returned, once, and the notifier must wake the guest.
    static int16_t long_pcm[48000][6];
    for (int i = 0; i < 48000; i++) long_pcm[i][0] = 1;
    gbuf long_blocks[2] = {{long_pcm, sizeof long_pcm, sizeof long_pcm}, {long_pcm, sizeof long_pcm, sizeof long_pcm}};
    assert(h_au_Append(out, long_blocks)); assert(h_au_Append(out, long_blocks + 1));
    uint64_t start = atomic_load(&ao(out)->cb_n), deadline = now_ns() + 5000000000ull;
    while (atomic_load(&ao(out)->cb_n) < start + 2) { assert(now_ns() < deadline); usleep(1000); }
    int old_signals = atomic_load(&signals);
    h_au_Stop(out); wait_for(&signals, old_signals + 1);
    assert(h_au_Released(out) == long_blocks && h_au_Released(out) == long_blocks + 1 && !h_au_Released(out));
    h_au_Close(out);
    printf("AudioOut: %d exact stereo frames from 6 channels, 3 played + 2 stop releases, %d notifications\n", TOTAL, atomic_load(&signals));
}
static void test_reconnect(void) {
    static int16_t samples[2400 * 2];
    gbuf blocks[8]; void *key = dm_pcmout_open(2); aout *a = ao(key);
    atomic_store(&reject_channels, 1);
    dm_pcmout_start(key); wait_backend(a, BK_NULL); wait_backend(a, BK_AU);
    uint64_t before = atomic_load(&a->cb_n);
    for (int i = 0; i < 8; i++) {
        blocks[i] = (gbuf){samples, sizeof samples, sizeof samples};
        assert(dm_pcmout_append(key, blocks + i));
    }
    uint64_t limit = now_ns() + 5000000000ull;
    while (atomic_load(&a->cb_n) == before) { assert(now_ns() < limit); usleep(1000); }
    // An injected route error on a genuinely running stream. Its callback only
    // publishes the error; the production watchdog closes it on its own thread.
    pthread_mutex_lock(&g_aos_lock);
    assert(a->backend == BK_AU && error_callback);
    error_callback(a->au, a, AAUDIO_ERROR_DISCONNECTED);
    pthread_mutex_unlock(&g_aos_lock);
    wait_backend(a, BK_NULL);
    for (int i = 0; i < 8; i++) {
        gbuf *b;
        while (!(b = dm_pcmout_released(key))) { assert(now_ns() < limit); dm_pcmout_wait(key, 20); }
        assert(b == blocks + i);
    }
    assert(!dm_pcmout_released(key)); wait_backend(a, BK_AU);
    dm_audio_suspend(1); wait_backend(a, BK_NULL);
    uint64_t stopped = atomic_load(&a->cb_n);
    assert(dm_pcmout_append(key, blocks));
    gbuf *released;
    while (!(released = dm_pcmout_released(key))) { assert(now_ns() < limit); dm_pcmout_wait(key, 20); }
    assert(released == blocks && !dm_pcmout_released(key));
    assert(atomic_load(&a->cb_n) == stopped);
    dm_audio_suspend(0); wait_backend(a, BK_AU);
    while (atomic_load(&a->cb_n) == stopped) { assert(now_ns() < limit); usleep(1000); }
    dm_pcmout_close(key);
    puts("AAudio: rejected channel mismatch, reopened, delivered 8 FIFO blocks across injected disconnect, resumed device callbacks after suspend");
}

// Minimal SDK side for the public renderer open and close. Its manual execute
// supplies a different two-channel DeviceSink command payload every 5 ms.
static int32_t mix[2 * BLOCK]; static int sdk_frames;
static void sdk_exec(void *obj, void *state) {
    (void)obj; (void)state;
    uint8_t cl[0x20 + 0x148] __attribute__((aligned(8))) = {0};
    *(uint32_t *)(cl+4) = 1; *(int32_t **)(cl+8) = mix;
    *(uint32_t *)(cl+0x14) = BLOCK; *(uint32_t *)(cl+0x18) = 48000;
    uint8_t *cmd = cl+0x20;
    *(uint32_t *)cmd = 0xcafebabe; cmd[4] = 1; cmd[5] = 0x13;
    *(uint16_t *)(cmd+6) = 0x148; *(uint32_t *)(cmd+0x130) = 2; *(uint16_t *)(cmd+0x136) = 1;
    int amplitude = ++sdk_frames;
    for (int i = 0; i < BLOCK; i++) { mix[i] = amplitude; mix[BLOCK+i] = -amplitude; }
    g_frame_n = capture(cl, sizeof cl, g_frame); assert(g_frame_n == BLOCK);
}
static Result sdk_open(void **h, const uint8_t *p, void *w, size_t sz) {
    assert(*(int32_t *)(p + P_DEVICE) == 1 && *(int32_t *)(p + P_MODE) == 1 && sz == WORK_PAGE);
    h[0] = w; h[1] = NULL; return RES_OK;
}
static Result sdk_open_event(void **h, void *ev, const uint8_t *p, void *w, size_t sz) { abort(); }
static void sdk_close(void *obj, void *state) { (void)obj; (void)state; }
void *guest_sym(const char *name) {
    if (!strcmp(name, hle_force_h_ar_Open0.name)) return sdk_open;
    if (!strcmp(name, hle_force_h_ar_Open.name)) return sdk_open_event;
    if (!strcmp(name, hle_force_h_ar_Close.name)) return sdk_close;
    if (!strcmp(name, "_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE")) return sdk_exec;
    return NULL;
}
static pthread_t render_thread;
static void *run_renderer(void *unused) { (void)unused; render_main(NULL); return NULL; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack) {
    assert(fn == render_main && !arg && prio == 9 && stack >= 0x10000);
    assert(!pthread_create(&render_thread, NULL, run_renderer, NULL)); return &render_thread;
}
void dm_guest_thread_join(void *t) { assert(t == &render_thread && !pthread_join(render_thread, NULL)); }
static void test_renderer(void) {
    atomic_store(&heard_frames, 0); atomic_store(&skip_leading_silence, 1);
    uint8_t param[P_SIZE] = {0}; static uint8_t work[WORK_PAGE];
    *(uint32_t *)param = 48000; *(uint32_t *)(param+4) = BLOCK; *(uint32_t *)(param+P_DEVICE) = 1;
    void *handle[2];
    assert(h_ar_Open0(handle, param, work, sizeof work) == RES_OK);
    wait_for(&heard_frames, CAPTURE); h_ar_Close(handle[0], handle[1]);
    for (int i = 0; i < CAPTURE; i++) {
        int amplitude = i / BLOCK + 1;
        if (heard[2*i] != amplitude || heard[2*i+1] != -amplitude) {
            fprintf(stderr, "renderer sample %d expected %d / %d, got %d / %d, max callback %d frames, produced %d blocks\n", i, amplitude, -amplitude, heard[2*i], heard[2*i+1], atomic_load(&max_frames), sdk_frames);
            abort();
        }
    }
    assert(!key && sdk_frames >= CAPTURE / BLOCK);
    printf("AudioRenderer: %d consecutive 5 ms DeviceSink blocks (%d stereo frames) at AAudio callback\n", CAPTURE / BLOCK, CAPTURE);
}
int main(void) {
    unsetenv("DM_AUDIO"); unsetenv("DM_AUDIO_MUTE");
    test_guest(); test_reconnect(); test_renderer();
    printf("PASS (%llu device callbacks)\n", (unsigned long long)dm_audio_render_calls());
    return 0;
}
