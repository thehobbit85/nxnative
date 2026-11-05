// Offline friends contracts. Real registration records and kernel event
// lifetimes; only the SDK's small SystemEvent ABI wrapper is synthetic.
#include "../hle_friends.c"
#include <assert.h>
#include <unistd.h>

int svc_thread(int, uint64_t *);
uint32_t svc_thread_main_init(void);
void thr_counts(int *, int *, int *);
typedef struct { uint32_t wh, rh; } sdk_event;
static void *seen[40]; static unsigned nseen;
static void mark(void *fn) { for (unsigned i = 0; i < nseen; i++) if (seen[i] == fn) return; assert(nseen < 40); seen[nseen++] = fn; }
#define RUN(fn, ...) (mark((void *)fn), fn(__VA_ARGS__))
static uint32_t create(void *p, int mode, int interprocess) {
    assert(mode == 0 && interprocess);
    uint64_t r[32] = {0}; assert(svc_thread(0x45, r) && !r[0]);
    *(sdk_event *)p = (sdk_event){r[1], r[2]}; return 0;
}
static uint32_t writable(const void *p) { return ((const sdk_event *)p)->wh; }
void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")) return create;
    if (!strcmp(name, "_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE")) return writable;
    fprintf(stderr, "Unexpected SDK lookup: %s\n", name); abort();
}
static void event_state(sdk_event *ev, int ready) {
    uint64_t r[32] = {0, (uintptr_t)&ev->rh, 1, 0}; assert(ev->wh && ev->rh);
    assert(svc_thread(0x18, r) && r[0] == (ready ? 0 : 0xea01));
}
static void close_event(sdk_event *ev) {
    uint64_t r[32] = {ev->wh}; assert(svc_thread(0x16, r) && !r[0]);
    r[0] = ev->rh; assert(svc_thread(0x16, r) && !r[0]);
}
static void unchanged(const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) assert(b[i] == 0xa5); }
int main(void) {
    alarm(20); svc_thread_main_init(); int h0, l0, o0; thr_counts(&h0, &l0, &o0);
    uint8_t self[256], out[512], uid[16], ids[32], filter[32], desc[64];
    memset(self, 0xa5, sizeof self); memset(out, 0xa5, sizeof out); memset(uid, 0xa5, sizeof uid);
    memset(ids, 0xa5, sizeof ids); memset(filter, 0xa5, sizeof filter); memset(desc, 0xa5, sizeof desc);
    RUN(h_fr_Initialize); RUN(h_fr_AcCtor, self);
    for (unsigned k = 0; k < 64; k++) {
        struct { uint64_t before; sdk_event ev; uint64_t after; } guarded = {123, {0}, 456};
        assert(!RUN(h_fr_AcGetEv, self, &guarded.ev)); event_state(&guarded.ev, 1);
        uint64_t binding = hle_system_event_binding(&guarded.ev); assert(binding);
        sdk_event first = guarded.ev;
        assert(!RUN(h_fr_AcGetEv, self, &guarded.ev)); assert(!memcmp(&first, &guarded.ev, sizeof first));
        assert(hle_system_event_binding(&guarded.ev) == binding); event_state(&guarded.ev, 1);
        assert(RUN(h_fr_AcResult, self) == 0x2a2 && guarded.before == 123 && guarded.after == 456);
        close_event(&guarded.ev); assert(!hle_system_event_binding(&guarded.ev));
    }
    RUN(h_fr_AcDtor, self);
    for (int size = -1; size <= 4; size++) {
        int counts[3] = {123, 99, 456};
        assert(RUN(h_fr_GetFriendList, counts + 1, out, -1, size, filter) == 0x2a2 && counts[1] == 0);
        counts[1] = 99; assert(RUN(h_fr_GetBlocked4, counts + 1, out, -1, size) == 0 && !counts[1]);
        counts[1] = 99; assert(RUN(h_fr_GetBlocked, counts + 1, out, uid, -1, size) == 0x2a2 && !counts[1]);
        assert(counts[0] == 123 && counts[2] == 456); unchanged(out, sizeof out);
    }
    assert(RUN(h_fr_GetFriendList, NULL, out, 0, 0, filter) == 0x2a2);
    assert(!RUN(h_fr_GetBlocked4, NULL, out, 0, 0));
    assert(RUN(h_fr_GetBlocked, NULL, out, uid, 0, 0) == 0x2a2);
    assert(RUN(h_fr_GetProfileList, self, out, uid, ids, 2) == 0x2a2);
    assert(RUN(h_fr_UpdateFriendInfo, out, uid, ids, 2) == 0x2a2);
    assert(RUN(h_fr_AddPlayHistory, out, uid, ids) == 0x2a2);
    assert(RUN(h_fr_GetPHKey, out, uid, 1) == 0x2a2);
    RUN(h_fr_NqCtor, self); assert(RUN(h_fr_NqInit, self, uid) == 0x2a2);
    sdk_event *notification = RUN(h_fr_NqGetEv, self), *invitation = RUN(h_fr_InvEv);
    assert(notification != invitation && RUN(h_fr_NqGetEv, self) == notification && RUN(h_fr_InvEv) == invitation);
    sdk_event n0 = *notification, i0 = *invitation;
    for (unsigned k = 0; k < 64; k++) {
        event_state(notification, 0); event_state(invitation, 0);
        assert(RUN(h_fr_NqPop, self, out) == 0x2a2);
        uint64_t size = UINT64_C(0xa5a5a5a5a5a5a5a5);
        assert(!RUN(h_fr_InvPop, uid, &size, out, sizeof out)); unchanged(&size, sizeof size);
        assert(!memcmp(notification, &n0, sizeof n0) && !memcmp(invitation, &i0, sizeof i0));
    }
    RUN(h_fr_NqDtor, self); event_state(notification, 0); close_event(notification); close_event(invitation);
    assert(RUN(h_fr_StartInv, uid, 1, desc, out, sizeof out) == 0x2a2);
    assert(RUN(h_fr_SendInv, uid, ids, 2, desc, out, sizeof out) == 0x2a2);
    assert(RUN(h_fr_ShowDetail, uid, ids, filter, desc) == 0x2a2);
    assert(RUN(h_fr_ShowList, uid) == 0x2a2); assert(RUN(h_fr_ShowBlocked, uid) == 0x2a2);
    assert(RUN(h_fr_ShowRecv, uid) == 0x2a2); assert(RUN(h_fr_StartFaced, uid) == 0x2a2);
    assert(RUN(h_fr_StartSend, uid, ids, filter, desc) == 0x2a2); assert(RUN(h_fr_ShowSend, uid) == 0x2a2);
    assert(RUN(h_fr_OpenSession, uid) == 0x2a2); assert(RUN(h_fr_CloseSession, uid) == 0x2a2);
    assert(RUN(h_fr_UpInit, self, uid) == 0x2a2); assert(RUN(h_fr_UpDesc, self, "offline") == 0x2a2);
    assert(RUN(h_fr_UpCommit, self) == 0x2a2);
    unchanged(self, sizeof self); unchanged(out, sizeof out); unchanged(uid, sizeof uid);
    unchanged(ids, sizeof ids); unchanged(filter, sizeof filter); unchanged(desc, sizeof desc);
    unsigned long bytes; const hle_force_ent *entries = DM_SECTION_DATA(swforce, &bytes);
    unsigned records = bytes / sizeof *entries;
    for (unsigned i = 0; i < records; i++) {
        unsigned j = 0; while (j < nseen && entries[i].fn != seen[j]) j++;
        if (j == nseen) fprintf(stderr, "Untested friends registration: %s\n", entries[i].name);
        assert(j < nseen);
    }
    assert(records == 33 && nseen == 33);
    int h, l, o; thr_counts(&h, &l, &o); assert(h == h0 && l == l0 && o == o0);
    puts("Friends: all 33 imports, overload results, untouched outputs, completion and quiet event lifetimes passed");
    return 0;
}
