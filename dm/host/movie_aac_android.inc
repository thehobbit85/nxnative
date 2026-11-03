// Android's AAC decoder consumes one raw access unit per input, with AudioSpecificConfig in csd-0.
// PCM can arrive later than its packet. Keep its own presentation time through EOS and seek.
typedef struct pcm_chunk {
    struct pcm_chunk *next;
    int64_t pts;
    size_t bytes;
    uint8_t pcm[];
} pcm_chunk;
static int aac_ready(const void *owner) { return ((const mdec_t *)owner)->pcm_first != NULL; }
static int aac_output(void *owner, AMediaFormat *f, const uint8_t *p, size_t n, int64_t pts) {
    mdec_t *d = owner;
    int ch = media_format_int(f, "channel-count", d->ch), rate = media_format_int(f, "sample-rate", d->rate);
    if (media_format_int(f, "pcm-encoding", 2) != 2 || ch < 1 || ch > 8 || rate <= 0 || n > MD_PCM_CAP || n % (2 * ch)) return 0;
    pcm_chunk *c = malloc(sizeof *c + n);
    if (!c) return 0;
    c->next = NULL; c->bytes = n; c->pts = pts; memcpy(c->pcm, p, n);
    if (d->pcm_last) d->pcm_last->next = c; else d->pcm_first = c;
    d->pcm_last = c;
    d->rate = rate; d->ch = ch;
    return 1;
}
static void md_aac_close(mdec_t *d) {
    media_close(&d->aac);
    while (d->pcm_first) { pcm_chunk *next = d->pcm_first->next; free(d->pcm_first); d->pcm_first = next; }
    d->pcm_last = NULL;
}
static int md_aac_open(mdec_t *d) {
    uint8_t asc[64]; int n = md_asc(d, asc);
    AMediaFormat *f = AMediaFormat_new();
    if (!f) return 0;
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, d->rate);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, d->ch);
    AMediaFormat_setInt32(f, "pcm-encoding", 2);
    AMediaFormat_setBuffer(f, "csd-0", asc, n);
    d->aac = (media_stream){.codec=AMediaCodec_createDecoderByType("audio/mp4a-latm"), .owner=d, .copy=aac_output, .ready=aac_ready};
    int ok = d->aac.codec && AMediaCodec_configure(d->aac.codec, f, NULL, NULL, 0) == AMEDIA_OK && AMediaCodec_start(d->aac.codec) == AMEDIA_OK;
    AMediaFormat_delete(f);
    return ok;
}
static int md_aac(mdec_t *d, const uint8_t *p, int n, int slot) {
    if (!d->aac.codec && !n) return 0;
    if (!d->aac.codec && !md_aac_open(d)) goto failed;
    if (n && !media_feed(&d->aac, p, (size_t)n, d->jpts, 0)) goto failed;
    if (d->jflush) {
        if (!media_finish(&d->aac, 0)) goto failed;
    } else if (!d->pcm_first) {
        int r;
        do {
            r = media_drain(&d->aac, 10000);
            if (r < 0) goto failed;
        } while (r && !d->pcm_first);
    }
    pcm_chunk *c = d->pcm_first;
    if (!c) return 0;
    if (d->out[slot].cap < c->bytes) {
        uint8_t *buf = realloc(d->out[slot].frm, c->bytes);
        if (!buf) goto failed;
        d->out[slot].frm = buf; d->out[slot].cap = c->bytes;
    }
    memcpy(d->out[slot].frm, c->pcm, c->bytes);
    d->out[slot].bytes = c->bytes; d->jpts_out = c->pts;
    d->pcm_first = c->next; if (!d->pcm_first) d->pcm_last = NULL;
    free(c); return 1;
failed:
    if (!d->red_dec++) LOG("[RED] movie: MediaCodec AAC configuration, input or PCM output failed");
    md_aac_close(d); return 0;
}
