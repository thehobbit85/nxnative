// SDK buffer and event adapters for the public movie decoder tests. Include after hle_movie.c.
typedef struct { uint8_t *p; size_t cap, size; } buffer;
static void set_data(void *b, void *p, size_t n) { *(buffer *)b = (buffer){p,n,0}; }
static void set_range(void *b, int off, int n) {
    buffer *v = b; assert(!off && n >= 0 && (size_t)n <= v->cap); v->size = n;
}
static void *data(const void *b) { return ((const buffer *)b)->p; }
static size_t cap(const void *b) { return ((const buffer *)b)->cap; }
static size_t size(const void *b) { return ((const buffer *)b)->size; }

static _Atomic uint64_t events[4];
static void sdk_signal(void *ev) { atomic_fetch_add((_Atomic uint64_t *)ev, 1); }
static void *event_ptrs[] = {events, events+1, events+2, events+3};
__thread _Atomic int dm_host_work;
uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void *guest_sym(const char *name) {
    static uint64_t vt[4]; assert(!strcmp(name, "_ZTVN5movie7DecoderE")); return vt;
}
void dm_host_lock(pthread_mutex_t *m) { assert(!pthread_mutex_lock(m)); }
#ifdef __APPLE__
void dm_host_helper_qos(qos_class_t q) { (void)q; }
#endif
static void *deferred(void *arg) { md_defer_body(arg); return NULL; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int priority, size_t stack_size) {
    assert(fn == md_defer_body && !arg && priority == 16 && stack_size == (64 << 10));
    pthread_t t; assert(!pthread_create(&t, NULL, deferred, arg)); assert(!pthread_detach(t));
    return (void *)1;
}
