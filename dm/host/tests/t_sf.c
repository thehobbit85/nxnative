// Host service sessions (hle_sf.c) without a guest: a CMIF request on a served service (olsc:u) succeeds with zeroed output and no special header; the camera context of the Switch 2
// (nn::camera::sdk::CreateCameraContext) is a host session with no camera: Initialize's command 3 and Finalize's 4 succeed, 21 copies an event's read end to the client in the special header,
// and the device commands (QueryDeviceConnection's 29, any other) fail with 0x7c4, "no camera device" (Star Fox's camera module and its game code read it so; the earlier 0xdc4 read as "no camera support")
#include "../hle.h"
#include "../hle_sf.c"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
int svc_thread(int, uint64_t *);
uint32_t svc_thread_main_init(void);
void thr_counts(int *, int *, int *);
static void *seen[16]; static unsigned nseen;
static void mark(void *fn) { for (unsigned i = 0; i < nseen; i++) if (seen[i] == fn) return; assert(nseen < 16); seen[nseen++] = fn; }
#define RUN(fn, ...) (mark((void *)fn), fn(__VA_ARGS__))
static void close_event(uint32_t h) {
    uint64_t r[32] = {0, (uintptr_t)&h, 1, 0};
    CHECK(svc_thread(0x18, r) && r[0] == 0xea01, "camera event is unsignalled");
    r[0] = h; CHECK(svc_thread(0x16, r) && !r[0], "copied event handle closes");
}

// A CMIF request for command cmd on a non-domain session (HIPC type 4, no special header, no descriptors): the data words from +8, "SFCI" at +0x10
static Result request(uint32_t h, uint32_t *m, unsigned cmd) {
    memset(m, 0xcc, 0x100);
    m[0] = 4; m[1] = 8;
    m[4] = 0x49434653; m[5] = 0; m[6] = cmd; m[7] = 0;
    return RUN(h_sf_Send, h, m, 0x100);
}
static void malformed_domain(void) {
    size_t ps = (size_t)getpagesize();
    uint8_t *pages = mmap(NULL, ps * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(pages != MAP_FAILED && !mprotect(pages + ps, ps, PROT_NONE));
    uint32_t *m = (uint32_t *)(pages + ps - 0x40), h, obj;
    assert(!RUN(h_sf_Connect, &h, "olsc:u") && !RUN(h_sf_ToDomain, &obj, h));
    memset(m, 0, 0x40); m[0] = 0xffff0004; m[1] = 8;
    uint8_t before[0x40]; memcpy(before, m, sizeof before);
    CHECK(RUN(h_sf_Send, h, m, 0x40) == 0xe401, "out-of-buffer domain descriptors rejected before reading");
    CHECK(!memcmp(before, m, sizeof before), "invalid request remains untouched");
    const uint32_t counts[] = {15u << 16, 15u << 20, 15u << 24, 15u << 28, 0};
    for (unsigned k = 0; k < sizeof counts / sizeof *counts; k++) {
        memset(m, 0xa5, 0x40); m[0] = 4 | counts[k]; m[1] = 8 | (k == 4 ? 1u << 31 : 0); m[2] = 0x3ff;
        memcpy(before, m, sizeof before);
        CHECK(RUN(h_sf_Send, h, m, 0x40) == 0xe401 && !memcmp(before, m, sizeof before), "each descriptor or special-header overflow is rejected without writes");
    }
    for (unsigned ndw = 0; ndw < 10; ndw++) {
        memset(m, 0xa5, 0x40); m[0] = 4; m[1] = ndw; m[4] = 0x100001; m[5] = 1;
        m[8] = 0x49434653; m[9] = 1; m[10] = 21; m[11] = 0;
        memcpy(before, m, sizeof before);
        CHECK(RUN(h_sf_Send, h, m, 0x40) == 0xe401 && !memcmp(before, m, sizeof before), "declared data must contain complete domain and CMIF headers");
    }
    for (unsigned ndw = 5; ndw <= 6; ndw++) {
        memset(m, 0xa5, 0x40); m[0] = 4; m[1] = ndw; m[4] = 2; m[5] = 1;
        memcpy(before, m, sizeof before);
        CHECK(RUN(h_sf_Send, h, m, 0x40) == (ndw == 6 ? 0 : 0xe401), "domain close exact header boundary");
        CHECK(ndw == 6 || !memcmp(before, m, sizeof before), "short domain close leaves input intact");
    }
    RUN(h_sf_Close, h); assert(!munmap(pages, ps * 2));
}
static void session_contracts(void) {
    const char *names[] = {"olsc:u", "caps:su", "caps:u", "caps:a", "caps:c", "caps:sc", "caps:ss"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        uint32_t h, obj = 0, dup; uint16_t pointer_size = 0xffff;
        CHECK(!RUN(h_sf_Connect, &h, names[i]), "all served names connect");
        CHECK(!RUN(h_sf_PtrBuf, &pointer_size, h) && !pointer_size, "host sessions have no pointer buffer");
        CHECK(!RUN(h_sf_ToDomain, &obj, h) && obj == 1, "domain object is one");
        CHECK(!RUN(h_sf_DupEx, &dup, h, 23) && dup != h, "tagged duplicate has independent lifetime");
        RUN(h_sf_Close, h);
        uint32_t m[0x40]; memset(m, 0xa5, sizeof m);
        m[0] = 6; m[1] = 10; m[4] = 0x100001; m[5] = 1; m[6] = 0; m[7] = 77;
        m[8] = 0x49434653; m[9] = 1; m[10] = 7; m[11] = 0;
        CHECK(!RUN(h_sf_Send, dup, m, sizeof m), "domain duplicate survives closing source");
        CHECK(!m[4] && !m[5] && !m[6] && !m[7] && m[8] == 0x4f434653 && m[9] == 1 && !m[10] && !m[11], "domain response header and version");
        for (unsigned k = 12; k < 0x40; k++) CHECK(!m[k], "offline output is zeroed");
        memset(m, 0xa5, sizeof m); m[0] = 4; m[1] = 8; m[4] = 2; m[5] = 1;
        CHECK(!RUN(h_sf_Send, dup, m, sizeof m), "domain close needs no SFCI header");
        CHECK(!m[0] && !m[1] && !m[2] && !m[3] && m[4] == 2, "domain close clears only response header");
        RUN(h_sf_Close, dup);
        uint32_t out = 0xa5a5a5a5; pointer_size = 0xa5a5;
        CHECK(RUN(h_sf_Dup, &out, dup) == 0x2a2 && out == 0xa5a5a5a5, "closed duplicate rejected, output untouched");
        CHECK(RUN(h_sf_DupEx, &out, 0, 0) == 0x2a2 && out == 0xa5a5a5a5, "invalid tagged duplicate rejected");
        CHECK(RUN(h_sf_ToDomain, &out, dup) == 0x2a2 && out == 0xa5a5a5a5, "closed domain conversion rejected");
        CHECK(RUN(h_sf_PtrBuf, &pointer_size, dup) == 0x2a2 && pointer_size == 0xa5a5, "closed pointer query rejected");
        uint32_t before[0x40]; memcpy(before, m, sizeof m);
        CHECK(RUN(h_sf_Send, dup, m, sizeof m) == 0xe401 && !memcmp(before, m, sizeof m), "closed send rejected without buffer mutation");
        RUN(h_sf_Close, dup); // A repeated close has no effect on other sessions.
    }
    uint32_t h, m[0x40]; CHECK(!RUN(h_sf_Connect, &h, "olsc:u"), "malformed non-domain session");
    for (unsigned ndw = 0; ndw <= 6; ndw++) {
        memset(m, 0xa5, sizeof m); m[0] = 4; m[1] = ndw; m[4] = 0x49434653; m[5] = 0; m[6] = 1; m[7] = 0;
        uint32_t before[0x40]; memcpy(before, m, sizeof m);
        CHECK(RUN(h_sf_Send, h, m, sizeof m) == (ndw == 6 ? 0 : 0xe401), "non-domain CMIF exact header boundary");
        CHECK(ndw == 6 || !memcmp(before, m, sizeof m), "short non-domain header leaves input intact");
    }
    for (unsigned mode = 0; mode < 4; mode++) {
        memset(m, 0xa5, sizeof m); m[0] = mode == 0 ? 5 : 4; m[1] = mode == 3 ? 0x3ff : 8;
        m[4] = mode == 1 ? 0 : 0x49434653;
        uint32_t before[0x40]; memcpy(before, m, sizeof m);
        CHECK(RUN(h_sf_Send, h, m, mode == 2 ? 0x3f : sizeof m) == 0xe401 && !memcmp(before, m, sizeof m), "unsupported type, bad magic, short buffer or data overflow rejected");
    }
    // Valid special header plus X/A/B/W descriptors are skipped; the answer is still descriptor-free.
    memset(m, 0xa5, sizeof m); m[0] = 0x11110004; m[1] = 8 | (1u << 31); m[2] = 1 | 2 | 32;
    // 8-byte header + 4 special + 8 PID + 4 copy + 4 move + 8 X + 3*12 A/B/W = 72; align to 80.
    m[20] = 0x49434653; m[21] = 1; m[22] = 0; m[23] = 0;
    CHECK(!RUN(h_sf_Send, h, m, sizeof m) && m[4] == 0x4f434653 && m[5] == 1 && !m[6] && !(m[1] >> 31), "valid descriptors and handles locate CMIF correctly");
    RUN(h_sf_Close, h);
}
int main(void) {
    svc_thread_main_init();
    malformed_domain();
    session_contracts();
    int start_h, start_live, start_objects; thr_counts(&start_h, &start_live, &start_objects);
    uint32_t m[0x40], h = 0;
    CHECK(RUN(h_sf_InitRes) == 0, "service resolution initializes");
    CHECK(RUN(h_applet_CamBegin) == RES_OK, "camera resource is available");
    CHECK(RUN(h_applet_CamEnd) == RES_OK, "camera resource releases");
    CHECK(RUN(h_cam_ContextEnd) == RES_OK, "camera context teardown succeeds");
    CHECK(RUN(h_sf_Connect, &h, "olsc:u") == RES_OK && h, "olsc:u is served");
    CHECK(request(h, m, 0) == RES_OK, "olsc:u request");
    CHECK(!(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "olsc:u: success, no special header, SFCO at +0x10");
    uint32_t untouched = 0xa5a5a5a5;
    CHECK(RUN(h_sf_Connect, &untouched, "chat:u") == 0xe15u && untouched == 0xa5a5a5a5, "unknown service leaves output untouched");

    uint32_t cam = 0;
    CHECK(RUN(h_cam_Context, &cam) == RES_OK && cam, "the camera context is a host session");
    CHECK(request(cam, m, 3) == RES_OK && m[4] == 0x4f434653 && m[6] == RES_OK && !(m[1] >> 31), "initializing the camera library: success");
    CHECK(request(cam, m, 29) == RES_OK && m[6] == 0x7c4u && !(m[1] >> 31), "querying the device: 0x7c4, no camera device");
    int nh, live, objects; thr_counts(&nh, &live, &objects);
    CHECK(nh == start_h, "no handle made for a failing command");
    CHECK(request(cam, m, 21) == RES_OK, "the camera's event");
    CHECK(m[1] >> 31 && (m[2] >> 1 & 15) == 1 && (m[2] & 1) == 0 && (m[2] >> 5 & 15) == 0, "special header: one copy handle, no process ID, no move handle");
    thr_counts(&nh, &live, &objects);
    CHECK(m[3] && nh == start_h + 1, "one readable event handle created");
    close_event(m[3]);
    CHECK(m[4] == 0x4f434653 && m[6] == RES_OK, "SFCO stays at +0x10 after the special header and handle");
    CHECK((m[1] & 0x3ff) == (0x100 - 0x10) / 4, "the data words start after the handle");
    for (unsigned k = 0; k < 128; k++) {
        CHECK(!request(cam, m, 21), "repeated camera event request"); close_event(m[3]);
        thr_counts(&nh, &live, &objects); CHECK(nh == start_h, "camera copied handles do not accumulate");
    }
    uint32_t dup = 0;
    CHECK(RUN(h_sf_Dup, &dup, cam) == RES_OK && dup && dup != cam, "a duplicate of the camera session");
    CHECK(request(dup, m, 5) == RES_OK && m[6] == 0x7c4u, "the duplicate answers as the camera");
    CHECK(request(dup, m, 4) == RES_OK && m[6] == RES_OK, "finalizing the camera library: success");
    RUN(h_sf_Close, dup); RUN(h_sf_Close, cam); RUN(h_sf_Close, h);
    RUN(h_sf_FinRes);
    thr_counts(&nh, &live, &objects);
    CHECK(nh == start_h && live == start_live && objects == start_objects, "camera event lifetime leaves no handles or threads");
    unsigned long bytes = 0; const hle_force_ent *entries = DM_SECTION_DATA(swforce, &bytes);
    CHECK(bytes / sizeof *entries == 13, "thirteen actual SF imports registered");
    for (size_t k = 0; k < bytes / sizeof *entries; k++) {
        unsigned j = 0; while (j < nseen && seen[j] != entries[k].fn) j++;
        CHECK(j < nseen, "every actual SF import invoked");
    }
    printf("t_sf: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
