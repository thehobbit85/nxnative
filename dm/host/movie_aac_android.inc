// Android's AAC decoder consumes one raw access unit per input, with AudioSpecificConfig in csd-0.
// PCM can arrive later than its packet. Keep its own presentation time through EOS and seek.
typedef struct pcm_chunk {
    struct pcm_chunk *next;
    int64_t pts;
    size_t bytes;
    uint8_t pcm[];
} pcm_chunk;
static int aac_ready(const void *owner) { return ((const mdec_t *)owner)->pcm_first != NULL; }
// The guest reads raw interleaved PCM without a speaker mask. AudioToolbox
// exposes MPEG element order for these ASC configurations; MediaCodec returns
// Android speaker order. Index by the stream configuration, since a PCE or
// a decoder downmix can have the same channel count and a different layout.
static const uint8_t *aac_guest_order(int config, int channels, int mask) {
    static const uint8_t center_first[] = {2,0,1,3,4};
    static const uint8_t surround[] = {2,0,1,4,5,3};
    static const uint8_t wide[] = {2,6,7,0,1,4,5,3};
    if (config >= 3 && config <= 5 && channels == config) return center_first;
    if (config == 6 && channels == 6) return surround;
    // For configuration 7, the Android 7.1 mask places the second front pair
    // after the rear pair. Apply this mapping only to the observed layout.
    if (config == 7 && channels == 8 && mask == 0x18fc) return wide;
    return NULL;
}
static int aac_output(void *owner, AMediaFormat *f, const uint8_t *p, size_t n, int64_t pts) {
    mdec_t *d = owner;
    int ch = media_format_int(f, "channel-count", d->ch), rate = media_format_int(f, "sample-rate", d->rate);
    if (media_format_int(f, "pcm-encoding", 2) != 2 || ch < 1 || ch > 8 || rate <= 0 || n > MD_PCM_CAP || n % (2 * ch)) return 0;
    pcm_chunk *c = malloc(sizeof *c + n);
    if (!c) return 0;
    c->next = NULL; c->bytes = n; c->pts = pts;
    const uint8_t *order = aac_guest_order(d->aac_channel_config, ch, media_format_int(f,"channel-mask",0));
    if (order) {
        for (size_t base = 0; base < n; base += 2*ch)
            for (int i = 0; i < ch; i++) memcpy(c->pcm + base + 2*i, p + base + 2*order[i], 2);
    } else memcpy(c->pcm, p, n);
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
static int asc_field(const uint8_t *asc, int n, int bit, int width) {
    if (bit + width > n*8) return -1;
    int value = 0;
    for (int i = 0; i < width; i++, bit++) value = (value << 1) | ((asc[bit/8] >> (7-bit%8)) & 1);
    return value;
}
static int md_aac_open(mdec_t *d) {
    uint8_t asc[64]; int n = md_asc(d, asc);
    // channelConfiguration follows audioObjectType (5 bits, or 11 for the
    // escape) and samplingFrequencyIndex (4 bits, plus 24 for an explicit Hz).
    int freq_bit = (asc[0] >> 3) == 31 ? 11 : 5;
    int config_bit = freq_bit + 4 + (asc_field(asc,n,freq_bit,4) == 15 ? 24 : 0);
    d->aac_channel_config = asc_field(asc,n,config_bit,4);
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
