// Real nn::web imports: no test-only host or Java entry points.
extern uint32_t web_start(void *, void **, const void *), web_wait(void *, void *), web_offline(void *, const void *);
extern unsigned web_appear(void *), web_send(void *, const char *, size_t), web_recv(void *, size_t *, char *, size_t);
extern void web_exit(void *);
uint32_t _ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb(void *event, int mode, int ipc) {
    (void)mode; (void)ipc;
    register uint64_t x0 __asm__("x0") = 0, x1 __asm__("x1") = 0, x2 __asm__("x2") = 0;
    __asm__ volatile("svc #0x45" : "+r"(x0), "+r"(x1), "+r"(x2) : : "memory");
    ((uint32_t *)event)[0] = x1; ((uint32_t *)event)[1] = x2; return x0;
}
uint32_t _ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE(const void *event) { return *(const uint32_t *)event; }
static int equal(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void web_arg(uint8_t *arg, const char *path, int hidden) {
    memset(arg, 0, 0x2000); *(uint16_t *)arg = 2;
    size_t n = 0; while (path[n]) n++; n = (n + 4) & ~(size_t)3;
    *(uint16_t *)(arg + 8) = 6; *(uint16_t *)(arg + 10) = n;
    // Copy only through the terminating zero; the rest is already padded.
    size_t k = 0; do { arg[16 + k] = path[k]; } while (path[k++]);
    *(uint16_t *)(arg + 16 + n) = 0x41; *(uint16_t *)(arg + 18 + n) = 4; *(uint32_t *)(arg + 24 + n) = hidden;
}
static void web_message_wait(const char *wanted, int held) {
    for (int tries = 0; tries < 600; tries++) {
        char buf[256] = {0}; size_t n = 0;
        if (held) { pad_state state; uint32_t id = 32; require(hid_read(&state, 1, &id) == 1 && !state.buttons); }
        if (web_recv(NULL, &n, buf, sizeof buf)) {
            NOTE("WEB_MESSAGE:"); message(buf, n); NOTE("\n");
            if (equal(buf, wanted)) return;
        }
        sleep_frame();
    }
    NOTE("WEB_MESSAGE_TIMEOUT\n"); require(0);
}
static void check_web(void) {
    _Alignas(16) uint8_t arg[0x2000];
    struct { uint64_t before, reason; char url[0x1000]; uint64_t size, after; } ret;
    void *event = NULL;
    web_arg(arg, "menu/index.html", 1);
    require(!web_start(NULL, &event, arg) && event);
    web_message_wait("READY:1280:resource", 0);
    require(web_send(NULL, "hello", 6)); web_message_wait("ECHO:hello", 0);
    require(web_appear(NULL)); NOTE("WEB_WAIT_A\n");
    web_message_wait("BUTTON_A", 1);
    web_exit(NULL); ret.before = 123; ret.after = 456;
    require(!web_wait(NULL, &ret.reason) && ret.reason == 2 && !ret.size && ret.before == 123 && ret.after == 456);
    NOTE("WEB_REQUEST_EXIT_OK\n");
    web_arg(arg, "menu/index.html", 0); require(!web_start(NULL, &event, arg));
    web_message_wait("READY:1280:resource", 1); NOTE("WEB_WAIT_B\n");
    require(!web_wait(NULL, &ret.reason) && ret.reason == 1); NOTE("WEB_BACK_OK\n");
    web_arg(arg, "menu/index.html", 0); require(!web_start(NULL, &event, arg));
    web_message_wait("READY:1280:resource", 1); NOTE("WEB_WAIT_SYSTEM_BACK\n");
    require(!web_wait(NULL, &ret.reason) && ret.reason == 1); NOTE("WEB_SYSTEM_BACK_OK\n");
    web_arg(arg, "menu/index.html", 0); require(!web_start(NULL, &event, arg));
    web_message_wait("READY:1280:resource", 1); require(web_send(NULL, "end", 4));
    web_message_wait("ENDING", 0); NOTE("WEB_WAIT_EXIT_INPUT\n");
    int input_returned = 0;
    for (int tries = 0; tries < 600; tries++) {
        pad_state state; uint32_t id = 32;
        if (hid_read(&state, 1, &id) == 1 && (state.buttons & 4)) { input_returned = 1; break; }
        sleep_frame();
    }
    require(input_returned); require(!web_wait(NULL, &ret.reason) && ret.reason == 0); NOTE("WEB_EXIT_INPUT_OK\n");
    web_arg(arg, "menu/callback.html", 0);
    require(!web_offline(&ret.reason, arg) && ret.reason == 3 && equal(ret.url, "http://localhost/finished") && ret.size == 26);
    NOTE("WEB_CALLBACK_OK\n");
    web_arg(arg, "menu/end.html", 0); require(!web_offline(&ret.reason, arg) && ret.reason == 0 && !ret.size);
    NOTE("WEB_SCRIPT_EXIT_OK\n");
    web_arg(arg, "menu/http_error.html", 0); require(!web_offline(&ret.reason, arg) && ret.reason == 0 && !ret.size);
    NOTE("WEB_HTTP_FAILURE_OK\n");
    web_arg(arg, "menu/missing.html", 0); require(web_offline(&ret.reason, arg) == 0x2a2);
    NOTE("WEB_FIXTURE_DONE\n");
}
