// Guest audio metadata and SDK entry points shared by the PCM fixtures.
typedef struct { int ch, rate; buffer csd[2]; } metadata;
static void set_i32(void *m, const char *key, int value) {
    metadata *f = m;
    if (!strcmp(key, "channel-count")) f->ch = value;
    else { assert(!strcmp(key, "sample-rate")); f->rate = value; }
}
static int find_i32(const void *m, const char *key, int *value) {
    const metadata *f = m;
    if (!strcmp(key, "channel-count")) *value = f->ch;
    else { assert(!strcmp(key, "sample-rate")); *value = f->rate; }
    return *value != 0;
}
static int find_buf(const void *m, const char *key, void **b) {
    const metadata *f = m;
    int i = !strcmp(key,"csd-0") ? 0 : !strcmp(key,"csd-1") ? 1 : -1;
    if (i < 0 || !f->csd[i].size) return 0;
    *b = (void *)&f->csd[i]; return 1;
}
int nvn_host_write_texture(const void *tex, const void *src, int w, int h, size_t row, int *tw, int *th) { abort(); }
int nvn_host_signal_sync(void *sync) { abort(); }

