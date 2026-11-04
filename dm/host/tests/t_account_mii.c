// Account/Mii contracts shared by Mac and Android. SDK event construction is
// synthetic; binding, signalling, waiting and handle release use the real host.
#include "../hle_account.c"
#include "../hle_mii.c"
#include <assert.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int svc_thread(int, uint64_t *);
uint32_t svc_thread_main_init(void);
void thr_counts(int *, int *, int *);
typedef struct { uint32_t wh, rh; } sdk_event;
static unsigned applet_depth, applet_enters;
void dm_applet_enter(const char *name) { assert(name && !applet_depth); applet_depth++; applet_enters++; }
void dm_applet_leave(void) { assert(applet_depth == 1); applet_depth--; }
uint64_t dm_entropy64(void) { static uint64_t value = 123; return ++value; }
static uint32_t sdk_create(void *p, int mode, int interprocess) {
    (void)mode; assert(interprocess);
    uint64_t r[32] = {0}; assert(svc_thread(0x45, r) && !r[0]);
    *(sdk_event *)p = (sdk_event){r[1], r[2]}; return 0;
}
static uint32_t sdk_writable(const void *p) { return ((const sdk_event *)p)->wh; }
void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")) return sdk_create;
    if (!strcmp(name, "_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE")) return sdk_writable;
    return NULL;
}
static void *seen[128]; static unsigned nseen;
static void mark(void *fn) {
    for (unsigned i = 0; i < nseen; i++) if (seen[i] == fn) return;
    assert(nseen < 128); seen[nseen++] = fn;
}
#define RUN(fn, ...) (mark((void *)fn), fn(__VA_ARGS__))
static void unchanged(const void *p, size_t n) {
    const uint8_t *b = p; for (size_t i = 0; i < n; i++) assert(b[i] == 0xa5);
}
static void event_complete(sdk_event *ev) {
    uint64_t r[32] = {0, (uintptr_t)&ev->rh, 1, 0};
    assert(ev->wh && ev->rh && svc_thread(0x18, r) && !r[0]);
    r[0] = ev->wh; assert(svc_thread(0x16, r) && !r[0]);
    r[0] = ev->rh; assert(svc_thread(0x16, r) && !r[0]);
    *ev = (sdk_event){0};
}
static void account(void) {
    RUN(h_acc_Init);
    int n; assert(!RUN(h_acc_UserCount, &n) && n == 1);
    Uid users[2], uid, other = {{3, 4}}; memset(users, 0xa5, sizeof users);
    assert(!RUN(h_acc_ListOpen, &n, users, 0) && n == 0); unchanged(users, sizeof users);
    assert(!RUN(h_acc_ListOpen, &n, users, 2) && n == 1); uid = users[0];
    unchanged(&users[1], sizeof users[1]); assert(uid.v[0] && uid.v[1]);
    assert(!RUN(h_acc_ListAll, &n, NULL, 0) && n == 0);
    assert(!RUN(h_acc_ListAll, &n, users, 2) && n == 1 && !memcmp(users, &uid, sizeof uid));
    assert(!RUN(h_acc_ListQual, &n, NULL, 4) && n == 0);
    assert(!RUN(h_acc_ListQual, &n, users, 2) && n == 1); unchanged(&users[1], sizeof users[1]);
    uint8_t exists = 2;
    assert(!RUN(h_acc_Exists, &exists, &uid) && exists == 1);
    assert(!RUN(h_acc_Exists, &exists, &other) && exists == 0);
    UserHandle handle;
    for (int i = 0; i < 32; i++) {
        assert(RUN(h_acc_TryOpenPre, &handle) == 1);
        assert(!RUN(h_acc_GetUserId, &users[0], &handle) && !memcmp(users, &uid, sizeof uid));
        RUN(h_acc_CloseUser, &handle);
        assert(!RUN(h_acc_OpenPre, &handle)); RUN(h_acc_CloseUser, &handle);
        assert(!RUN(h_acc_OpenUser, &handle, &other));
        assert(!RUN(h_acc_GetUserId, &users[0], &handle) && !memcmp(users, &other, sizeof other));
        RUN(h_acc_CloseUser, &handle);
    }
    assert(!RUN(h_acc_LastOpened, users) && !memcmp(users, &uid, sizeof uid));
    assert(!RUN(h_acc_ShowSel, users, NULL) && !memcmp(users, &uid, sizeof uid));
    assert(!RUN(h_acc_ShowCreator));
    memset(&handle, 0xa5, sizeof handle); RUN(h_acc_PopOpen, &n, &handle, 1);
    assert(n == 0); unchanged(&handle, sizeof handle); RUN(h_acc_PushOpen);
    struct { char text[33]; uint8_t guard[8]; } nickname;
    memset(&nickname, 0xa5, sizeof nickname); unsetenv("DM_NICK");
    assert(!RUN(h_acc_GetNick, nickname.text, &uid) && !strcmp(nickname.text, "Player"));
    unchanged(nickname.guard, sizeof nickname.guard);
    setenv("DM_NICK", "0123456789012345678901234567890123456789", 1);
    assert(!RUN(h_acc_GetNick, nickname.text, &uid) && strlen(nickname.text) == 32);
    assert(!memcmp(nickname.text, "01234567890123456789012345678901", 32));
    unchanged(nickname.guard, sizeof nickname.guard); unsetenv("DM_NICK");
    uint64_t bytes;
    assert(!RUN(h_acc_LoadImage, &bytes, NULL, 0, &uid) && bytes == 670);
    uint8_t jpeg[700]; memset(jpeg, 0xa5, sizeof jpeg);
    assert(!RUN(h_acc_LoadImage, &bytes, jpeg, 13, &uid) && bytes == 670);
    assert(jpeg[0] == 0xff && jpeg[1] == 0xd8); unchanged(jpeg + 13, sizeof jpeg - 13);
    assert(!RUN(h_acc_LoadImage, &bytes, jpeg, sizeof jpeg, &uid));
    assert(jpeg[668] == 0xff && jpeg[669] == 0xd9); unchanged(jpeg + 670, 30);
    assert(!RUN(h_acc_StoreThumb, &uid, jpeg, bytes)); assert(!RUN(h_acc_DeleteThumb, &uid));
    assert(!RUN(h_acc_NsaAvail, &exists, &handle) && !exists);
    assert(RUN(h_acc_EnsureNsa, &handle) == 0x2a2);
    memset(jpeg, 0xa5, sizeof jpeg);
    assert(RUN(h_acc_GetNsaId, jpeg, &handle) == 0x2a2);
    assert(RUN(h_acc_LoadTok, &bytes, (char *)jpeg, sizeof jpeg, &handle) == 0x2a2 && !bytes);
    assert(RUN(h_acc_ShowLic, &handle) == 0x2a2);
    assert(RUN(h_acc_LoadNaInfo, jpeg, &handle, jpeg, sizeof jpeg) == 0x2a2);
    unchanged(jpeg, sizeof jpeg);
    uint8_t context[128], other_context[128]; memset(context, 0xa5, sizeof context);
    sdk_event ev = {0};
    for (int i = 0; i < 32; i++) {
        RUN(h_acc_AcCtor, context);
        assert(RUN(h_acc_EnsureTokAsync, context, &handle) == 0x2a2);
        assert(!RUN(h_acc_AcHasDone, context, &exists) && exists);
        assert(!RUN(h_acc_AcGetEv, context, &ev)); event_complete(&ev);
        assert(RUN(h_acc_AcGetRes, context) == 0x2a2);
        assert(!RUN(h_acc_AcCancel, context));
        assert(RUN(h_acc_AcMove, context, other_context) == context);
        RUN(h_acc_AcDtor, context);
        RUN(h_acc_NaCtor, context);
        assert(RUN(h_acc_NaCreate, context, &handle, NULL, jpeg, sizeof jpeg) == 0x2a2);
        assert(!RUN(h_acc_NaGetEv, context, &ev)); event_complete(&ev);
        assert(RUN(h_acc_NaGetRes, context) == 0x2a2);
        assert(RUN(h_acc_NaGetTok, context, &bytes, (char *)jpeg, sizeof jpeg) == 0x2a2 && !bytes);
        assert(RUN(h_acc_NaGetCode, context, &bytes, (char *)jpeg, sizeof jpeg) == 0x2a2 && !bytes);
        assert(RUN(h_acc_NaGetState, context, &bytes, (char *)jpeg, sizeof jpeg) == 0x2a2 && !bytes);
        RUN(h_acc_NaDtor, context); unchanged(context, sizeof context); unchanged(jpeg, sizeof jpeg);
        int handles, live, objects; thr_counts(&handles, &live, &objects);
        assert(handles == 1 && live == 1 && objects == 1);
    }
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { UserHandle invalid = {{0}}; h_acc_GetUserId(&uid, &invalid); _exit(0); }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
static void check_mii(const mii_charinfo *m, int gender) {
    assert((m->create_id[6] & 0xf0) == 0x40 && (m->create_id[8] & 0xc0) == 0x80);
    static const uint16_t name[11] = {'n','o',' ','n','a','m','e',0,0,0,0};
    assert(!memcmp(m->nickname, name, sizeof name));
    assert(m->gender == gender && m->height == 64 && m->build == 64);
    assert(m->hair_color == 1 && m->eye_color == 8 && m->mouth_color == 19 && !m->reserved);
}
static void mii(void) {
    uint8_t database[64]; memset(database, 0xa5, sizeof database);
    assert(!RUN(h_mii_Init, database)); assert(RUN(h_mii_Avail, database));
    assert(!RUN(h_mii_Updated, database, 3) && !RUN(h_mii_Full, database));
    assert(RUN(h_mii_Count, database, 2) == 6 && !RUN(h_mii_Count, database, 1));
    mii_charinfo list[7]; memset(list, 0xa5, sizeof list); int count = -1;
    assert(!RUN(h_mii_Get, database, &count, list, 7, 1) && !count); unchanged(list, sizeof list);
    assert(!RUN(h_mii_Get, database, &count, list, 7, 2) && count == 6);
    unchanged(&list[6], sizeof list[6]);
    struct { mii_charinfo info; uint32_t source; } elements[7];
    _Static_assert(sizeof elements[0] == 0x5c, "CharInfoElement stride");
    memset(elements, 0xa5, sizeof elements);
    assert(!RUN(h_mii_GetE, database, &count, elements, 2, 2) && count == 2);
    unchanged(&elements[2], 5 * sizeof elements[0]);
    assert(!RUN(h_mii_GetE, database, &count, elements, 7, 3) && count == 6);
    unchanged(&elements[6], sizeof elements[6]);
    for (int i = 0; i < 6; i++) {
        mii_charinfo one; assert(!RUN(h_mii_Default, database, &one, i));
        assert(!memcmp(&one, &list[i], sizeof one) && !memcmp(&one, &elements[i].info, sizeof one));
        assert(elements[i].source == 1 && one.favorite_color == i * 2); check_mii(&one, i >= 3);
        for (int j = 0; j < i; j++) assert(memcmp(list[j].create_id, one.create_id, 16));
    }
    mii_charinfo random[2]; int desc[3] = {0, 0, 0};
    assert(!RUN(h_mii_Random, database, &random[0], desc)); desc[1] = 1;
    assert(!RUN(h_mii_Random, database, &random[1], desc));
    check_mii(&random[0], 0); check_mii(&random[1], 1);
    assert(memcmp(random[0].create_id, random[1].create_id, 16));
    assert(RUN(h_mii_Index, database, &count, list) == 0x87e && count == -1);
    mii_charinfo before = list[0];
    assert(RUN(h_mii_Latest, database, list, 2) == 0x87e && !memcmp(&before, list, sizeof before));
    assert(!RUN(h_mii_Format, database));
    assert(RUN(h_mii_Import, database, list, sizeof list) == 0x1987e);
    assert(RUN(h_mii_Export, database, list, sizeof list) == 0x1987e);
    count = 123; assert(RUN(h_mii_Append, &count) == 0xe7e && count == 123);
    assert(!RUN(h_mii_Edit)); RUN(h_mii_Fin, database); unchanged(database, sizeof database);
}
int main(void) {
    alarm(15); struct rlimit limit = {0, 0}; setrlimit(RLIMIT_CORE, &limit);
    svc_thread_main_init(); account(); mii();
    assert(!applet_depth && applet_enters == 4);
    unsigned long bytes; const hle_force_ent *entries = DM_SECTION_DATA(swforce, &bytes);
    unsigned records = bytes / sizeof *entries;
    for (unsigned i = 0; i < records; i++) {
        unsigned j; for (j = 0; j < nseen; j++) if (entries[i].fn == seen[j]) break;
        if (j == nseen) fprintf(stderr, "Untested account/Mii registration: %s\n", entries[i].name);
        assert(j < nseen);
    }
    assert(records == 59 && nseen == 59);
    puts("Account/Mii: all 59 registrations, output bounds, errors, identities and async event lifetimes: OK");
}
