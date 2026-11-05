// Shared offline stand-ins: result/output contracts and real kernel events.
// Deliberately synthetic SDK vtable/event wrappers; no guest assets required.
#include "../hle_misc.c"
#include "../hle_sdk16.c"
#include "../hle_offline.c"
#include <assert.h>
#include <sys/wait.h>

int svc_thread(int, uint64_t *);
uint32_t svc_thread_main_init(void);
void thr_counts(int *, int *, int *);
struct dm_flags g_flag;
module_t *g_mod; int g_nmod;
uint64_t g_t0_ns;
__thread _Atomic int dm_host_work;
#ifdef __APPLE__
pthread_key_t g_key18;
#else
void *dm_slot_get(unsigned key) { (void)key; return NULL; }
#endif
void crash_thread_init(void) {}
void guest_thread_init(void) {}
void guest_thread_set_id(uint64_t id) { (void)id; }
void guest_gate_stack(int own_stack) { (void)own_stack; }
void dm_thread_enter(uint64_t e, uint64_t a, uint64_t s) { (void)e; (void)a; (void)s; abort(); }
uint64_t now_ns(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t)); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
int dm_guest_as_contains(uintptr_t addr) { (void)addr; return 0; }
int dm_sdk_major(void) { return 20; }
int dm_game_imports(const char *prefix) { (void)prefix; return 1; }
void dm_backtrace(uint64_t fp) { (void)fp; abort(); }
uint32_t hle_hid_connected_npad(void) { return 0; }
uint32_t dm_native_image_blank(uint32_t w, uint32_t h) { (void)w; (void)h; abort(); }
void dm_native_image_blank_free(uint32_t h) { (void)h; abort(); }
static unsigned applet_depth, applets;
void dm_applet_enter(const char *name) { assert((!strcmp(name, "shop") || !strcmp(name, "error")) && !applet_depth); applet_depth++; applets++; }
void dm_applet_leave(void) { assert(applet_depth == 1); applet_depth--; }
typedef struct { uint32_t wh, rh; } sdk_event;
static uint32_t create_event(void *p, int mode, int ipc) {
    assert(mode == 0 && ipc); uint64_t r[32] = {0}; assert(svc_thread(0x45, r) && !r[0]);
    *(sdk_event *)p = (sdk_event){r[1], r[2]}; return 0;
}
static uint32_t writable_event(const void *p) { return ((const sdk_event *)p)->wh; }
static void check_event(void *p, int ready) {
    sdk_event *ev = p; uint64_t r[32] = {0, (uintptr_t)&ev->rh, 1, 0};
    assert(ev->wh && ev->rh && svc_thread(0x18, r) && r[0] == (ready ? 0 : 0xea01));
}
static void close_event(void *p) {
    sdk_event *ev = p; uint64_t r[32] = {ev->wh}; assert(svc_thread(0x16, r) && !r[0]);
    r[0] = ev->rh; assert(svc_thread(0x16, r) && !r[0]);
}
static const char *const ngc_names[] = {
    "_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj",
    "_ZN2nn3ngc15ProfanityFilterD1Ev",
    "_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj",
    "_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm",
    "_ZN2nn3ngc15ProfanityFilter8FinalizeEv",
    "_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm",
    "_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm",
    "_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj",
    "_ZN2nn3ngc15ProfanityFilterD0Ev",
};
static void *sdk_vtable[14]; static module_t sdk;
module_t *module_of(const void *p) { uintptr_t a = (uintptr_t)p; return p == sdk_vtable || (a >= 0x100000 && a < 0x110000) ? &sdk : NULL; }
void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")) return create_event;
    if (!strcmp(name, "_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE")) return writable_event;
    if (!strcmp(name, "_ZTVN2nn3ngc15ProfanityFilterE")) return sdk_vtable;
    for (unsigned i = 0; i < sizeof ngc_names / sizeof *ngc_names; i++) if (!strcmp(name, ngc_names[i])) return (void *)(uintptr_t)(0x100000 + 0x100 * i);
    fprintf(stderr, "Unexpected SDK lookup: %s\n", name); abort();
}
static void *seen[192]; static unsigned nseen;
static void mark(void *fn) { for (unsigned i = 0; i < nseen; i++) if (seen[i] == fn) return; assert(nseen < 192); seen[nseen++] = fn; }
#define RUN(fn, ...) (mark((void *)fn), fn(__VA_ARGS__))
static void unchanged(const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) assert(b[i] == 0xa5); }
static void shop(void) {
    uint8_t self[128], response[128], work[128], output[128];
    memset(self, 0xa5, sizeof self); memset(response, 0xa5, sizeof response); memset(work, 0xa5, sizeof work); memset(output, 0xa5, sizeof output);
    assert(!RUN(h_ec_InitSsa, work, sizeof work)); RUN(h_ec_SsaCtor, self); RUN(h_ec_ArCtor, response);
    assert(RUN(h_ec_SsaInit, self, work) == 0x2a2);
    assert(RUN(h_ec_SsaRequest, self, response, work, 1, "/offline", work, -1) == 0x2a2);
    assert(RUN(h_ec_ArTryWait, response) == 1);
    size_t sizes[3] = {123, 99, 456}; assert(RUN(h_ec_ArGetSize, response, sizes + 1) == 0x2a2 && !sizes[1] && sizes[0] == 123 && sizes[2] == 456);
    assert(RUN(h_ec_ArGetSize, response, NULL) == 0x2a2);
    assert(RUN(h_ec_ArGet, response, output, sizeof output) == 0x2a2);
    RUN(h_ec_ArCancel, response); assert(!RUN(h_ec_ArErr, response));
    RUN(h_ec_ArDtor, response); RUN(h_ec_SsaDtor, self); RUN(h_ec_FinSsa);
    unchanged(self, sizeof self); unchanged(response, sizeof response); unchanged(work, sizeof work); unchanged(output, sizeof output);
    assert(RUN(h_nsd_Resolve, output, work) == 0x2a2 && RUN(h_nsd_ResolveEx, output, work) == 0x2a2); unchanged(output, sizeof output);
    uint8_t env[10]; memset(env, 0xa5, sizeof env); RUN(h_nsd_EnvId, (char *)env + 1);
    assert(env[0] == 0xa5 && env[9] == 0xa5 && !memcmp(env + 1, "lp1\0\0\0\0\0", 8));
    unsigned long bytes; const hle_force_ent *entries = DM_SECTION_DATA(swforce, &bytes); unsigned count = 0;
    for (size_t i = 0; i < bytes / sizeof *entries; i++) if (!strncmp(entries[i].name, "_ZN2nn2ec", 9) && strstr(entries[i].name, "ShowShop") && !strstr(entries[i].name, "ForUpgrade")) {
        unsigned before = applets; mark(entries[i].fn); ((void (*)(void))entries[i].fn)();
        assert(!applet_depth && applets == before + 1); count++;
    }
    assert(count == 31);
    unsigned before = applets; RUN(h_ec_ShowUpg, 1); RUN(h_ec_ShowUpgU, 2, self);
    assert(!applet_depth && applets == before + 2);
}
static void profanity(void) {
    sdk_vtable[1] = (void *)0x1234;
    for (unsigned i = 0; i < 9; i++) sdk_vtable[2 + i] = (void *)(uintptr_t)(0x100000 + 0x100 * i);
    sdk_vtable[11] = (void *)0x101000; // Unknown SDK method: the documented zero-result fallback.
    sdk_vtable[12] = h_ngc_Ver; // A slot already bound to a host function stays intact.
    uint8_t self[64]; memset(self, 0xa5, sizeof self); RUN(h_ngc_Ctor, self);
    void **vt; memcpy(&vt, self, sizeof vt); assert(vt && vt != sdk_vtable + 2 && !vt[-2] && vt[-1] == (void *)0x1234);
    unchanged(self + sizeof vt, sizeof self - sizeof vt);
    assert(!RUN(h_ngc_Init, self, NULL, 0) && !RUN(h_ngc_Ver, self)); RUN(h_ngc_MaskMode, self, 5);
    assert(!RUN(h_ngc_Init3, self, NULL, 0, 0x100));
    uint32_t masks[5] = {123, 99, 99, 99, 456}; const char *words[] = {"a", "b", "c"};
    assert(!RUN(h_ngc_Check, self, masks + 1, ~0u, words, 3));
    assert(masks[0] == 123 && !masks[1] && !masks[2] && !masks[3] && masks[4] == 456);
    masks[1] = masks[2] = masks[3] = 99; assert(!RUN(h_ngc_Check16, self, masks + 1, ~0u, NULL, 3));
    assert(masks[0] == 123 && !masks[1] && !masks[2] && !masks[3] && masks[4] == 456);
    assert(!RUN(h_ngc_Check16, self, NULL, 0, NULL, 0));
    char text[] = "kept"; uint16_t text16[] = {'k', 'e', 'p', 't', 0}; int masked[3] = {123, 99, 456};
    assert(!RUN(h_ngc_Mask8, self, masked + 1, text, 1) && !masked[1]); masked[1] = 99;
    assert(!RUN(h_ngc_Mask16, self, masked + 1, text16, 1) && !masked[1]); masked[1] = 99;
    assert(!RUN(h_ngc_Mask16c, self, masked + 1, text16, 1) && !masked[1]);
    uint8_t found[3] = {123, 99, 45}; assert(!RUN(h_ngc_CheckCn, found + 1, text));
    assert(found[0] == 123 && !found[1] && found[2] == 45 && !RUN(h_ngc_CheckCn, NULL, text));
    assert(!RUN(h_ngc_MaskCn, text));
    const unsigned mask_slots[] = {0, 2, 7}, check_slots[] = {3, 5, 6}, void_slots[] = {1, 4, 8};
    for (unsigned i = 0; i < 3; i++) {
        masked[1] = 99;
        assert(!((Result (*)(void *, int *, void *, uint32_t))vt[mask_slots[i]])(self, masked + 1, text16, 3) && !masked[1]);
        masks[1] = masks[2] = masks[3] = 99;
        assert(!((Result (*)(void *, uint32_t *, uint32_t, const void **, uint64_t))vt[check_slots[i]])(self, masks + 1, 0, NULL, 3));
        assert(masks[0] == 123 && !masks[1] && !masks[2] && !masks[3] && masks[4] == 456);
        ((void (*)(void *))vt[void_slots[i]])(self);
    }
    assert(!((Result (*)(void))vt[9])() && vt[10] == (void *)h_ngc_Ver);
    assert(masked[0] == 123 && masked[2] == 456 && !strcmp(text, "kept") && text16[0] == 'k' && text16[3] == 't' && !text16[4]);
    RUN(h_ngc_Fini, self); RUN(h_ngc_Dtor, self);
}
static void streamplay(void) {
    uint8_t out[256]; memset(out, 0xa5, sizeof out);
    assert(RUN(h_stpl_MemSize, 0, 0, 4) == 0x1000 && !RUN(h_stpl_Init, 0, 0, out, sizeof out));
    RUN(h_stpl_CreateDisplays, 4); assert(!RUN(h_stpl_NativeWindow, 1)); RUN(h_stpl_DestroyDisplays);
    void *ev = RUN(h_stpl_ConnEv); assert(RUN(h_stpl_ConnEv) == ev); check_event(ev, 0);
    uint32_t state[3] = {123, 99, 456}; RUN(h_stpl_ConnState, state + 1); assert(state[0] == 123 && !state[1] && state[2] == 456);
    const uint32_t values[] = {0, 1, 0xa5a5a5a5, UINT32_MAX};
    for (unsigned i = 0; i < 4; i++) {
        RUN(h_stpl_SetStyles, values[i]); RUN(h_stpl_SetHold, values[3 - i]);
        assert(RUN(h_stpl_GetStyles) == values[i] && RUN(h_stpl_GetHold) == values[3 - i]);
    }
    RUN(h_stpl_HidFeat, ~0u); assert(!RUN(h_stpl_PadStyle, state));
    assert(!RUN(h_stpl_PadFull, out, 4, state) && !RUN(h_stpl_PadHandheld, out, 4, state));
    assert(!RUN(h_stpl_PadDual, out, 4, state) && !RUN(h_stpl_PadRight, out, 4, state) && !RUN(h_stpl_PadLeft, out, 4, state));
    assert(!RUN(h_stpl_SixHandles, out, 4, state, ~0u) && !RUN(h_stpl_SixStates, out, 4, state));
    assert(!RUN(h_stpl_VibHandles, out, 4, state, ~0u)); uint64_t info[3] = {123, 99, 456}; RUN(h_stpl_VibInfo, info + 1, state);
    assert(info[0] == 123 && !info[1] && info[2] == 456);
    assert(!RUN(h_stpl_JcmsHandle, state, state) && !RUN(h_stpl_JcmsStates, out, 4, state));
    RUN(h_stpl_Fini); unchanged(out, sizeof out); check_event(ev, 0); close_event(ev);
}
static void infrared(void) {
    uint8_t out[256], config[64]; memset(out, 0xa5, sizeof out); memset(config, 0xa5, sizeof config);
    uint32_t pad = 0x123400ff, handle = RUN(h_irs_Handle, &pad); assert(handle == 0xff);
    RUN(h_irs_Init, &handle); assert(RUN(h_irs_Status, &handle) == 2);
    assert(RUN(h_irs_RunMoment, &handle, config) == 0x2a2 && RUN(h_irs_RunCluster, &handle, config) == 0x2a2 && RUN(h_irs_RunHand, &handle, config) == 0x2a2);
    assert(RUN(h_irs_RunImage, &handle, config, out, sizeof out) == 0x2a2 && RUN(h_irs_RunImageEx, &handle, config, out, sizeof out) == 0x2a2);
    int counts[3] = {123, 99, 456};
    assert(RUN(h_irs_Moment, out, counts + 1, 4, &handle) == 0x2a2 && !counts[1]); counts[1] = 99;
    assert(RUN(h_irs_Cluster, out, counts + 1, 4, &handle) == 0x2a2 && !counts[1]); counts[1] = 99;
    assert(RUN(h_irs_HandImage, out, counts + 1, 4, 0, &handle) == 0x2a2 && !counts[1]); counts[1] = 99;
    assert(RUN(h_irs_HandSil, out, counts + 1, 4, 0, &handle) == 0x2a2 && !counts[1]);
    assert(counts[0] == 123 && counts[2] == 456);
    assert(RUN(h_irs_ImageState, out, out, sizeof out, &handle) == 0x2a2);
    RUN(h_irs_Stop, &handle); RUN(h_irs_Fini, &handle); unchanged(out, sizeof out); unchanged(config, sizeof config);
}
static void network_clock(void) {
    uint8_t self[128]; memset(self, 0xa5, sizeof self); RUN(h_time_NcCtor, self);
    assert(!RUN(h_time_NcInit, self, 0)); void *event = RUN(h_time_NcEvent, self); check_event(event, 1);
    assert(!RUN(h_time_NcStart, self) && RUN(h_time_NcResult, self) == 0x2a2 && RUN(h_time_NcEvent, self) == event);
    check_event(event, 1); RUN(h_time_NcDtor, self); close_event(event); unchanged(self, sizeof self);
    int64_t now = 0x123456789; assert(RUN(h_time_NetNow, &now) == 0x2a2 && now == 0x123456789);
}
static void other_contracts(void) {
    uint8_t out[128]; memset(out, 0xa5, sizeof out); int counts[3] = {123, 99, 456};
    RUN(h_fr_SetAlloc, NULL, NULL);
    assert(RUN(h_fr_List2, counts + 1, out, out, 0, 4, out) == 0x2a2 && !counts[1] && counts[0] == 123 && counts[2] == 456);
    assert(RUN(h_fr_List2, NULL, out, out, 0, 0, out) == 0x2a2);
    uint8_t flag[3] = {123, 99, 45}; assert(!RUN(h_fr_ListAvail, flag + 1, out) && !flag[1]);
    assert(RUN(h_fr_ListEnsure, out, out) == 0x2a2 && !RUN(h_fr_AcHasDone, out, flag + 1) && flag[1] == 1);
    assert(flag[0] == 123 && flag[2] == 45 && RUN(h_fr_PlayHist2, out, out, out, out) == 0x2a2); unchanged(out, sizeof out);
    assert(!RUN(h_pctl_GetStereo)); RUN(h_pctl_SetStereo, 0x101); assert(!RUN(h_pctl_GetStereo));
    assert(!RUN(h_pctl_ConfStereo) && !RUN(h_pctl_ReqStereo));
    const unsigned args[] = {0, 1, 0x100, 0x101, UINT32_MAX};
    for (unsigned i = 0; i < 5; i++) {
        assert(!RUN(h_pctl_CheckFree, args[i]) && !RUN(h_pctl_TryBegin, args[i]) && !RUN(h_pctl_Stereo, args[i]));
        RUN(h_pctl_End); assert(!RUN(h_pctl_IsFree));
    }
    unsigned before = applets;
    RUN(h_err_ShowError, 0x2a2); RUN(h_err_ShowCode, UINT64_C(0x200000001)); RUN(h_err_ShowVar, out);
    RUN(h_err_AocVer); RUN(h_err_AppVer); RUN(h_err_ShowApp, out); RUN(h_err_ShowNet, UINT64_C(0x300000002));
    RUN(h_aoc_LostErr, counts, 2); assert(!applet_depth && applets == before + 8); unchanged(out, sizeof out);
    uint64_t stats[8], ids[] = {0x100, 0x200}; memset(stats, 0xa5, sizeof stats);
    assert(!RUN(h_pdm_QueryStats, stats + 1, out, ids, 2));
    unchanged(stats, 8); unchanged(stats + 7, 8);
    assert(stats[1] == 0x100 && !stats[2] && !stats[3] && stats[4] == 0x200 && !stats[5] && !stats[6]);
    memset(stats, 0xa5, sizeof stats); assert(!RUN(h_pdm_QueryStats, stats, out, ids, -1)); unchanged(stats, sizeof stats);
}
static void nfc(void) {
    assert(!RUN(h_nfc_State) && !RUN(h_nfp_LibState) && !RUN(h_nfc_Enabled));
    for (unsigned cycle = 0; cycle < 32; cycle++) {
        RUN(h_nfp_Init); RUN(h_nfc_Init); assert(RUN(h_nfc_State) == 1 && RUN(h_nfp_LibState) == 1 && !RUN(h_nfc_Enabled));
        struct { uint64_t pre; NfpDeviceHandle device; uint64_t post; } g;
        memset(&g, 0xa5, sizeof g); int count[3] = {123,99,456};
        assert(!RUN(h_nfp_List, &g.device, count + 1, 0) && !count[1]); unchanged(&g, sizeof g);
        assert(!RUN(h_nfp_List, &g.device, count + 1, -1) && !count[1]); unchanged(&g, sizeof g);
        assert(!RUN(h_nfp_List, &g.device, count + 1, 1) && count[1] == 1 && count[0] == 123 && count[2] == 456);
        assert(g.pre == 0xa5a5a5a5a5a5a5a5ull && g.post == g.pre && g.device.id == 0x4e465030);
        for (unsigned i = 0; i < sizeof g.device.reserved; i++) assert(!g.device.reserved[i]);
        uint32_t pad[3] = {123,99,456}; assert(!RUN(h_nfp_NpadId, pad + 1, &g.device) && !pad[1] && pad[0] == 123 && pad[2] == 456);
        sdk_event events[3] = {0}; RUN(h_nfp_AttachAvail, events);
        assert(!RUN(h_nfp_AttachAct, events + 1, &g.device) && !RUN(h_nfp_AttachDeact, events + 2, &g.device));
        for (unsigned i = 0; i < 3; i++) check_event(events + i, 0);
        assert(!RUN(h_nfp_Start, &g.device) && RUN(h_nfp_State, &g.device) == 1);
        uint8_t out[128]; memset(out, 0xa5, sizeof out);
        const Result no_tag = 115u | (97u << 9);
        assert(RUN(h_nfp_TagInfo, out, &g.device) == no_tag && RUN(h_nfp_ModelInfo, out, &g.device) == no_tag);
        assert(RUN(h_nfp_Mount, &g.device, 0, 3) == no_tag && RUN(h_nfp_Mount2, &g.device, 0) == no_tag);
        assert(RUN(h_nfp_Unmount, &g.device) == no_tag && RUN(h_nfp_CommonInfo, out, &g.device) == no_tag);
        assert(RUN(h_nfp_RegInfo, out, &g.device) == no_tag && RUN(h_nfp_Flush, &g.device) == no_tag);
        assert(RUN(h_nfp_OpenArea, &g.device, 0x12345678) == no_tag);
        uint64_t size[3] = {123,99,456}; assert(RUN(h_nfp_GetArea, out, size + 1, &g.device, sizeof out) == no_tag);
        assert(!size[1] && size[0] == 123 && size[2] == 456 && RUN(h_nfp_GetArea, out, NULL, &g.device, 0) == no_tag);
        assert(RUN(h_nfp_SetArea, &g.device, out, sizeof out) == no_tag && RUN(h_nfp_CreateArea, &g.device, out) == no_tag);
        assert(RUN(h_nfp_RecreateArea, &g.device, out) == no_tag && RUN(h_nfp_Restorer, &g.device, out, out) == no_tag);
        assert(RUN(h_nfp_Eraser, &g.device, out, out) == no_tag);
        uint8_t changed[3] = {123,99,45}; assert(RUN(h_nfp_Nick, &g.device, changed + 1, out, out, out) == no_tag && !changed[1]);
        changed[1] = 99; assert(RUN(h_nfp_Nick2, &g.device, changed + 1, out, out, out, out) == no_tag && !changed[1]);
        assert(changed[0] == 123 && changed[2] == 45); unchanged(out, sizeof out);
        RUN(h_nfc_Fini); assert(!RUN(h_nfc_State) && RUN(h_nfp_State, &g.device) == 1);
        assert(!RUN(h_nfp_Stop, &g.device) && !RUN(h_nfp_State, &g.device));
        RUN(h_nfp_Fini); assert(!RUN(h_nfp_LibState));
        for (unsigned i = 0; i < 3; i++) { check_event(events + i, 0); close_event(events + i); }
    }
}
static void album(void) {
    uint8_t out[128]; memset(out, 0xa5, sizeof out); RUN(h_album_Init);
    assert(RUN(h_album_Save, out, sizeof out, 1, 0, 1) == 0x2a2 && RUN(h_album_Save7, out, sizeof out, 1, 0) == 0x2a2);
    assert(RUN(h_album_Share, out, sizeof out, 1, 0, 1, out, "caption") == 0x2a2);
    assert(RUN(h_album_ShareId, out, out, sizeof out, 1, 0, out, "caption") == 0x2a2); unchanged(out, sizeof out);
    const size_t sizes[] = {0,1,17};
    for (unsigned i = 0; i < 3; i++) {
        memset(out, 0xa5, sizeof out); assert(RUN(h_album_ShareUrl, (char *)out + 1, sizes[i], out + 64) == 0x2a2);
        assert(out[0] == 0xa5 && out[1] == (sizes[i] ? 0 : 0xa5)); unchanged(out + 2, sizeof out - 2);
    }
    assert(RUN(h_album_ShareUrl, NULL, 17, out) == 0x2a2 && RUN(h_album_Show) == 0x2a2 && RUN(h_album_SaveCur, 0) == 0x2a2);
    RUN(h_album_Fini);
}
static void aoc_case(int mode) {
    char dir[1024], path[1100]; const char *tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(dir, sizeof dir, "%s/dm-aoc-XXXXXX", tmp); assert(mkdtemp(dir));
    const char *names[] = {"20", "3.img", "3", "2000.img", "2001", "0", "-1", "junk", "4.tmp"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) { snprintf(path, sizeof path, "%s/%s", dir, names[i]); assert(!mkdir(path, 0700)); }
    setenv("DM_AOC_DIR", dir, 1); unsetenv("DM_AOC_OWNED");
    if (mode == 1) setenv("DM_AOC_OWNED", "0", 1); else if (mode == 2) unsetenv("DM_AOC_DIR");
    int (*count)(void) = hle_force_h_aoc_Count.fn;
    int (*list)(int *, int, int) = hle_force_h_aoc_List.fn;
    assert(count() == (mode ? 0 : 3)); const int values[] = {3,20,2000};
    for (int off = -1; off <= 4; off++) for (int cap = -1; cap <= 4; cap++) {
        int out[7]; memset(out, 0xa5, sizeof out); int start = off < 0 ? 0 : off;
        int n = mode || cap <= 0 || start >= 3 ? 0 : cap < 3 - start ? cap : 3 - start;
        assert(list(out + 1, off, cap) == n);
        unchanged(out, sizeof *out); unchanged(out + 1 + n, sizeof out - (1 + n) * sizeof *out);
        for (int i = 0; i < n; i++) assert(out[i + 1] == values[start + i]);
    }
    // Ownership is a process-start snapshot, even if files disappear later.
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) { snprintf(path, sizeof path, "%s/%s", dir, names[i]); assert(!rmdir(path)); }
    assert(!rmdir(dir) && count() == (mode ? 0 : 3));
}
static void aoc(void) {
    for (int mode = 0; mode < 3; mode++) {
        fflush(NULL); pid_t pid = fork(); assert(pid >= 0);
        if (!pid) { aoc_case(mode); _exit(0); }
        int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    int (*count)(void) = hle_force_h_aoc_Count.fn;
    int (*list)(int *, int, int) = hle_force_h_aoc_List.fn;
    unsetenv("DM_AOC_DIR"); unsetenv("DM_AOC_OWNED"); assert(!RUN(count) && !RUN(list, NULL, 0, 0));
    for (unsigned i = 0; i < 32; i++) { sdk_event event = {0}; RUN(h_aoc_GetEv, &event); check_event(&event, 0); close_event(&event); }
}
int main(void) {
    alarm(20); svc_thread_main_init(); int h0, l0, o0; thr_counts(&h0, &l0, &o0);
    shop(); profanity(); streamplay(); infrared(); network_clock(); other_contracts(); nfc(); album(); aoc();
    const char *prefixes[] = {"_ZN2nn3ngc", "_ZN2nn10streamplay", "_ZN2nn8irsensor", "_ZN2nn3nsd",
        "_ZN2nn2ec19ShopServiceAccessor", "_ZNK2nn2ec19ShopServiceAccessor", "_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask",
        "_ZN2nn7friends", "_ZN2nn4pctl", "_ZN2nn3err", "_ZN2nn3pdm", "_ZN2nn3nfc", "_ZN2nn3nfp", "_ZN2nn5album", "_ZN2nn3aoc"};
    unsigned long bytes; const hle_force_ent *entries = DM_SECTION_DATA(swforce, &bytes); unsigned checked = 0;
    for (size_t i = 0; i < bytes / sizeof *entries; i++) {
        int wanted = 0; for (unsigned p = 0; p < sizeof prefixes / sizeof *prefixes; p++) wanted |= !strncmp(entries[i].name, prefixes[p], strlen(prefixes[p]));
        if (!wanted) continue;
        unsigned j = 0; while (j < nseen && entries[i].fn != seen[j]) j++;
        if (j == nseen) fprintf(stderr, "Untested offline import: %s\n", entries[i].name);
        assert(j < nseen); checked++;
    }
    int h, l, o; thr_counts(&h, &l, &o); assert(h == h0 && l == l0 && o == o0);
    printf("Offline services: %u family registrations, shop applets, vtables, guards and event lifetimes passed\n", checked);
    return 0;
}
