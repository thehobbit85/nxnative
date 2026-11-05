// Drive the real diagnostic poller through kernel-readable shared memory.
// Each writer waits for its observed log entry before changing the next value.
#include "../probe.c"
#include <assert.h>
#include <sys/wait.h>
#include <time.h>

typedef struct { _Atomic int ready; _Atomic uint64_t value; } shared_t;
static shared_t *shared;
module_t *module_of(const void *p) { (void)p; return NULL; }
uint64_t g_t0_ns;
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
void dm_guest_heap_range(uint64_t *base, size_t *size) {
    if (base) *base = atomic_load(&shared->ready) ? (uintptr_t)&shared->value : 0;
    if (size) *size = sizeof shared->value;
}
void dm_probe_text_install(void) {}
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { (void)fn; abort(); }
void dm_backtrace(uint64_t fp) { (void)fp; abort(); }
uint32_t thr_cur_handle(void) { return 0; }
void svc_thread_dump(void) { fputs("PROBE_SERVICE_DUMP\n", stderr); }

static int dumps;
static void await_line(FILE *log, const char *needle) {
    char line[2048];
    while (fgets(line, sizeof line, log)) {
        if (strstr(line, "PROBE_SERVICE_DUMP")) dumps++;
        if (strstr(line, needle)) return;
        if (strstr(line, "[RED]")) fputs(line, stderr);
    }
    fprintf(stderr, "missing poll evidence: %s\n", needle); abort();
}
static void run(int heap, int filter, unsigned bytes) {
    shared = mmap(NULL, (size_t)getpagesize(), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(shared != MAP_FAILED); atomic_init(&shared->ready, 0); atomic_init(&shared->value, 0); dumps = 0;
    int pipefd[2]; assert(!pipe(pipefd));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        alarm(5); close(pipefd[0]); assert(dup2(pipefd[1], STDERR_FILENO) >= 0); close(pipefd[1]);
        setvbuf(stderr, NULL, _IONBF, 0); unsetenv("DM_PROBE_WATCH");
        char config[128];
        if (heap) snprintf(config, sizeof config, "heap+0+%x%s", bytes, filter ? "=3f" : "");
        else snprintf(config, sizeof config, "%llx+%x%s", (unsigned long long)(uintptr_t)&shared->value, bytes, filter ? "=3f" : "");
        setenv("DM_PROBE_POLL", config, 1); probe_install();
        fputs("PROBE_INSTALLED\n", stderr);
        for (;;) pause();
    }
    close(pipefd[1]); FILE *log = fdopen(pipefd[0], "r"); assert(log);
    await_line(log, "PROBE_INSTALLED"); atomic_store(&shared->ready, 1);
    await_line(log, "armed, value 0");
    for (unsigned i = 0; i < 20; i++) {
        unsigned value = i & 1 ? 0x3f : i + 1;
        atomic_store(&shared->value, value);
        char expected[64]; snprintf(expected, sizeof expected, "→ %#x", value);
        await_line(log, expected);
        unsigned matches = filter ? (i + 1) / 2 : i + 1;
        while (dumps < (int)(matches < 2 ? matches : 2)) await_line(log, "PROBE_SERVICE_DUMP");
        assert(dumps == (int)(matches < 2 ? matches : 2));
    }
    assert(!kill(child, SIGTERM)); int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    fclose(log); assert(!munmap(shared, (size_t)getpagesize()));
}
int main(void) {
    alarm(30);
    size_t page = (size_t)getpagesize();
    uint8_t *span = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(span != MAP_FAILED); memset(span, 0xa5, page);
    assert(!mprotect(span + page, page, PROT_NONE));
    g_plen = 8; g_poll = (uintptr_t)span + page - 4;
    uint64_t value = 0x1234; assert(!poll_read(&value) && value == 0x1234); // A partial read is not a change.
    g_poll = 0; assert(!poll_read(&value) && value == 0x1234);
    for (g_plen = 1; g_plen <= 8; g_plen *= 2) {
        g_poll = (uintptr_t)span; assert(poll_read(&value));
        uint64_t expected = 0; memset(&expected, 0xa5, (size_t)g_plen); assert(value == expected);
    }
    assert(!munmap(span, 2 * page));
    for (unsigned bytes = 1; bytes <= 8; bytes *= 2)
        for (int heap = 0; heap < 2; heap++) for (int filter = 0; filter < 2; filter++) run(heap, filter, bytes);
    puts("Memory poll: 320 observed changes, 1/2/4/8 bytes, absolute/late heap addresses, filters and bounded service dumps: OK");
}
