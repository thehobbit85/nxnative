// Synchronous byte-buffer input/output for the Android movie decoders. Drain output during input
// backpressure and copy it before the codec recycles the buffer. No guest thread owns codec memory.
#pragma once
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <time.h>
typedef struct {
    AMediaCodec *codec;
    int eos_in, eos_out;
    void *owner;
    int (*copy)(void *, AMediaFormat *, const uint8_t *, size_t, int64_t);
    int (*ready)(const void *);
} media_stream;
static void media_close(media_stream *s) {
    if (s->codec) { AMediaCodec_stop(s->codec); AMediaCodec_delete(s->codec); }
    *s = (media_stream){0};
}
static int media_format_int(AMediaFormat *f, const char *key, int fallback) {
    int32_t v; return AMediaFormat_getInt32(f, key, &v) ? v : fallback;
}
// One output, format change or EOS. The negative result signals a codec/layout failure.
static int media_drain(media_stream *s, int64_t wait_us) {
    AMediaCodecBufferInfo bi;
    ssize_t i = AMediaCodec_dequeueOutputBuffer(s->codec, &bi, wait_us);
    if (i == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return 0;
    if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return 1;
    if (i < 0) return -1;
    int ok = 1;
    if (bi.size > 0 && !(bi.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
        size_t cap = 0; uint8_t *src = AMediaCodec_getOutputBuffer(s->codec, (size_t)i, &cap);
        AMediaFormat *f = AMediaCodec_getBufferFormat(s->codec, (size_t)i);
        ok = src && f && bi.offset >= 0 && (size_t)bi.offset <= cap && (size_t)bi.size <= cap-bi.offset &&
             s->copy(s->owner, f, src+bi.offset, bi.size, bi.presentationTimeUs);
        if (f) AMediaFormat_delete(f);
    }
    if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) s->eos_out = 1;
    if (AMediaCodec_releaseOutputBuffer(s->codec, i, false) != AMEDIA_OK) ok = 0;
    return ok ? 1 : -1;
}
// The guest may be holding its movie lock while waiting for a decode. Fail a nonresponsive
// codec instead of leaving the worker and every thread waiting on that lock parked forever.
static int64_t codec_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int media_feed(media_stream *s, const uint8_t *p, size_t n, int64_t pts, uint32_t flags) {
    int64_t deadline = codec_now_ms() + 5000;
    do {
        ssize_t i = AMediaCodec_dequeueInputBuffer(s->codec, 10000);
        if (i >= 0) {
            size_t cap = 0; uint8_t *dst = AMediaCodec_getInputBuffer(s->codec, i, &cap);
            if (n && (!dst || n > cap)) return 0;
            if (n) memcpy(dst, p, n);
            return AMediaCodec_queueInputBuffer(s->codec, i, 0, n, pts, flags) == AMEDIA_OK;
        }
        if (i != AMEDIACODEC_INFO_TRY_AGAIN_LATER || media_drain(s, 0) < 0) return 0;
    } while (codec_now_ms() < deadline);
    return 0;
}
static int media_finish(media_stream *s, int all) {
    if (!s->eos_in) {
        if (!media_feed(s, NULL, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) return 0;
        s->eos_in = 1;
    }
    int64_t deadline = codec_now_ms() + 5000;
    while (!s->eos_out && (all || !s->ready(s->owner))) {
        int r = media_drain(s, 10000);
        if (r < 0 || (!r && codec_now_ms() >= deadline)) return 0;
        if (r) deadline = codec_now_ms() + 5000;
    }
    return 1;
}
