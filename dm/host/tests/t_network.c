// Shared offline SDK contracts on Apple and Android. The sleep boundary records
// the requested guest duration; actual deadlines are exercised by t_clock.
#define dm_guest_sleep_ns network_sleep
#include "../hle_net.c"
#undef dm_guest_sleep_ns
#undef SK_ENETDOWN
// Two SDK generations have different registrations with the same local names.
#define h_sk_Bind h_sk_BindPlain
#define h_sk_Poll h_sk_PollPlain
#define hle_force_h_sk_Bind hle_force_h_sk_BindPlain
#define hle_force_h_sk_Poll hle_force_h_sk_PollPlain
#include "../hle_offline.c"
#undef h_sk_Bind
#undef h_sk_Poll
#undef hle_force_h_sk_Bind
#undef hle_force_h_sk_Poll
#include <assert.h>
#include <sched.h>

int svc_thread(int, uint64_t *);
uint32_t svc_thread_main_init(void);
void thr_counts(int *, int *, int *);
static int sleeps;
static int64_t slept;
void network_sleep(int64_t ns) { sleeps++; slept = ns; }
void dm_applet_enter(const char *name) { assert(name); }
void dm_applet_leave(void) {}
typedef struct { uint32_t write, read; } sdk_event;
static uint32_t event_create(void *p, int mode, int interprocess) {
    (void)mode; assert(interprocess);
    uint64_t r[32] = {0}; assert(svc_thread(0x45, r) && !r[0]);
    *(sdk_event *)p = (sdk_event){r[1], r[2]}; return 0;
}
static uint32_t event_write(const void *p) { return ((const sdk_event *)p)->write; }
void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")) return event_create;
    if (!strcmp(name, "_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE")) return event_write;
    return NULL;
}
static void unchanged(const void *p, size_t n) {
    const uint8_t *b = p; for (size_t i = 0; i < n; i++) assert(b[i] == 0xa5);
}
static _Atomic int ready, go, held;
static void *request_thread(void *p) {
    int id = (int)(intptr_t)p;
    assert(h_nsk_GetErrno() == 50);
    h_nsk_SetErrno(100 + id);
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&go)) sched_yield();
    assert(h_sk_GetLastError() == 100 + id);
    atomic_fetch_add(&held, h_nifm_OnHold());
    assert(h_nsk_Socket(2, 1, 0) == -1);
    assert(h_nsk_GetErrno() == 50);
    return NULL;
}
static void requests(void) {
    assert(!h_nifm_Initialize() && !h_socket_Initialize(NULL));
    assert(!h_sk_Init4(NULL, 0, 0, 0) && !h_sk_Fini());
    assert(!h_nifm_Available() && !h_nifm_HandleErr());
    h_nifm_Submit(); assert(h_nifm_OnHold() && !h_nifm_OnHold());
    h_nifm_Submit(); h_nifm_Cancel(); assert(!h_nifm_OnHold());
    h_nsk_SetErrno(99); h_nifm_Submit();
    pthread_t threads[8];
    for (intptr_t i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, request_thread, (void *)i));
    while (atomic_load(&ready) != 8) sched_yield();
    atomic_store(&go, 1);
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&held) == 1 && !h_nifm_OnHold());
    assert(h_nsk_GetErrno() == 99 && h_sk_Errno() == 50);
    uint32_t ip[7]; memset(ip, 0xa5, sizeof ip);
    assert(h_nifm_GetIp(&ip[1]) == RES_FAIL && !ip[1]);
    assert(h_nifm_GetIp10(&ip[1]) == RES_FAIL && !ip[1]);
    assert(h_nifm_IpCfg(&ip[1], &ip[2], &ip[3], &ip[4], &ip[5]) == RES_FAIL);
    for (int i = 1; i < 6; i++) assert(!ip[i]);
    memset(ip + 1, 0xa5, 5 * sizeof *ip);
    assert(h_nifm_IpCfg10(&ip[1], &ip[2], &ip[3], &ip[4], &ip[5]) == RES_FAIL);
    for (int i = 1; i < 6; i++) assert(!ip[i]);
    unchanged(ip, 4); unchanged(ip + 6, 4);
    memset(ip, 0xa5, sizeof ip); assert(h_nifm_GetProxy(ip) == RES_FAIL); unchanged(ip, sizeof ip);
    h_nifm_LocalMode(1); h_nifm_SubmitWait(); assert(!h_nifm_Available());
    char env[10]; memset(env, 0xa5, sizeof env); h_nsd_EnvId(env + 1);
    assert(!memcmp(env + 1, "lp1\0\0\0\0\0", 8)); unchanged(env, 1); unchanged(env + 9, 1);
}
static void waits(void) {
    sk_pollfd fd[3] = {{-1, 1, 7}, {-9, 2, 8}, {-1, 3, 9}};
    assert(!h_sk_Poll(fd, 3, 33) && slept == 33000000 && sleeps == 1);
    for (int i = 0; i < 3; i++) assert(!fd[i].revents);
    assert(!h_nsk_Poll(NULL, 0, 0) && slept == 0 && sleeps == 2);
    assert(!h_sk_PollPlain(NULL, 0, -1) && slept == 100000000 && sleeps == 3);
    fd[1] = (sk_pollfd){0, 2, 42};
    assert(h_sk_PollPlain(fd, 3, 99) == -1 && sleeps == 3 && fd[1].revents == 42);
    assert(h_nsk_GetErrno() == 50);
    uint64_t sets[16] = {0}; int64_t timeout[2] = {1, 23000};
    assert(!h_sk_Select(1024, sets, sets, sets, timeout) && slept == 1023000000);
    assert(!h_sk_Select2(0, NULL, NULL, NULL, NULL) && slept == 100000000);
    sets[15] = UINT64_C(1) << 63; int calls = sleeps;
    assert(h_sk_Select2(1024, sets, NULL, NULL, timeout) == -1 && sleeps == calls);
    assert(h_sk_Select(1024, NULL, sets, NULL, timeout) == -1 && sleeps == calls);
    assert(h_sk_Select(1024, NULL, NULL, sets, timeout) == -1 && sleeps == calls);
    assert(!h_sk_Select2(1023, sets, NULL, NULL, timeout) && sleeps == calls + 1);
}
static void sockets(void) {
    uint8_t out[64]; memset(out, 0xa5, sizeof out); unsigned size = 64;
    assert(h_sk_Socket(2, 1, 0) == -1 && h_sk_Socket3i(2, 1, 0) == -1);
#define PAIR(fn, ...) do { assert(h_sk_##fn(__VA_ARGS__) == -1); assert(h_nsk_##fn(__VA_ARGS__) == -1); } while (0)
    PAIR(Bind, 3, out, size); PAIR(Connect, 3, out, size); PAIR(Close, 3);
    PAIR(GetPeerName, 3, out, &size); PAIR(GetSockName, 3, out, &size);
    PAIR(GetSockOpt, 3, 0, 0, out, &size); PAIR(SetSockOpt, 3, 0, 0, out, size);
    PAIR(Fcntl, 3, 0); PAIR(Recv, 3, out, sizeof out, 0); PAIR(Send, 3, out, sizeof out, 0);
    PAIR(ResGet, out, 0); PAIR(ResSet, out);
#undef PAIR
    assert(h_sk_Accept(3, out, &size) == -1 && h_sk_Listen(3, 2) == -1);
    assert(h_sk_RecvFrom(3, out, sizeof out, 0, out, &size) == -1);
    assert(h_sk_BindPlain(3, out, size) == -1 && h_sk_Connect2(3, out, size) == -1);
    assert(h_sk_Accept2(3, out, &size) == -1 && h_sk_GetSockName2(3, out, &size) == -1);
    assert(h_sk_GetSockOpt2(3, 0, 0, out, &size) == -1 && h_sk_SetSockOpt2(3, 0, 0, out, size) == -1);
    assert(h_sk_Fcntl2(3, 0) == -1 && h_sk_Shutdown(3, 0) == -1);
    assert(h_sk_Recv2(3, out, sizeof out, 0) == -1 && h_sk_Send2(3, out, sizeof out, 0) == -1);
    assert(h_sk_SendTo(3, out, sizeof out, 0, out, size) == -1);
    assert(h_sk_SendTo2(3, out, sizeof out, 0, out, size) == -1);
    assert(h_sk_RecvFrom2(3, out, sizeof out, 0, out, &size) == -1);
    unsigned long value = 123;
    assert(h_sk_EventFd(0, 0) == -1 && h_sk_EventFdRead(3, &value) == -1 && value == 123);
    assert(h_sk_EventFdWrite(3, value) == -1);
    assert(size == 64); unchanged(out, sizeof out);
    void *ai = out; assert(h_sk_GetAddrInfo("example.invalid", "443", NULL, &ai) == 4 && !ai);
    ai = out; assert(h_sk_Gai2("example.invalid", "443", NULL, &ai) == 4 && !ai);
    h_sk_FreeAddrInfo(NULL); h_sk_FreeAi2(NULL); assert(*h_sk_GaiStr(4));
    assert(!h_sk_GetHostEnt("example.invalid", NULL, 0) && !h_nsk_GetHost("example.invalid", NULL, 0));
    assert(h_sk_GetLastError() == 50);
}
static void ssl(void) {
    uint8_t object[64], data[64]; memset(object, 0xa5, sizeof object); memset(data, 0xa5, sizeof data);
    assert(!h_ssl_Initialize() && !h_ssl_InitCpp() && !h_ssl_Finalize() && !h_ssl_FinCpp());
    assert(h_ssl_CtxCreate(object, 0) == RES_FAIL && h_ssl_CtxDestroy(object) == RES_FAIL);
    uint64_t id = 99; assert(h_ssl_CtxRegPki(object, &id, 0) == RES_FAIL && id == 99);
    assert(h_ssl_CtxRemovePki(object, id) == RES_FAIL);
    assert(h_ssl_ImpCli(object, &id, NULL, NULL, 0, 0) == RES_FAIL && !id);
    id = 99; assert(h_ssl_ImpSrv(object, &id, NULL, 0, 0) == RES_FAIL && !id);
    id = 99; assert(h_ssl_ImpCrl(object, &id, NULL, 0) == RES_FAIL && !id);
    uint32_t size = 99; assert(h_ssl_BiBufSize(&size, NULL, 0) == RES_FAIL && !size);
    void *cert = data; assert(h_ssl_BiCerts(&cert, data, sizeof data, NULL, 0) == RES_FAIL && cert == data);
    assert(h_ssl_Certs(&cert, data, sizeof data, NULL, 0) == RES_FAIL && !cert);
    assert(h_ssl_CnCreate(object, object) == RES_FAIL && h_ssl_CnDestroy(object) == RES_FAIL);
    assert(h_ssl_CnHandshake(object) == RES_FAIL && h_ssl_CnHost(object, "example.invalid", 15) == RES_FAIL);
    assert(h_ssl_CnIoMode(object, 0) == RES_FAIL && h_ssl_CnCache(object, 0) == RES_FAIL && h_ssl_CnSock(object, 3) == RES_FAIL);
    uint32_t error = 0; assert(h_ssl_CnLastErr(object, &error) == RES_FAIL && error == RES_FAIL);
    error = 0; assert(h_ssl_CnVerifyErr(object, &error) == RES_FAIL && error == RES_FAIL);
    int bytes = 99; assert(h_ssl_CnPeek(object, (char *)data, &bytes, sizeof data) == RES_FAIL && !bytes);
    assert(h_ssl_CnPoll(object, data, data, 0) == RES_FAIL);
    assert(h_ssl_CnRead(object, (char *)data, sizeof data) == -1 && h_ssl_CnWrite(object, (char *)data, sizeof data) == -1);
    Result (*c_api[])(void) = {h_nssl_Create, h_nssl_Destroy, h_nssl_Handshake, h_nssl_GetId,
        h_nssl_GetAlpn, h_nssl_VerifyErr, h_nssl_Peek, h_nssl_Pending, h_nssl_Read, h_nssl_SetHost,
        h_nssl_SetIo, h_nssl_SetAlpn, h_nssl_SetOpt, h_nssl_SetCache, h_nssl_SetSock, h_nssl_SetVerify,
        h_nssl_Write, h_nssl_CtxDestroy, h_nssl_CtxGetId};
    for (unsigned i = 0; i < sizeof c_api / sizeof *c_api; i++) assert(c_api[i]() == RES_FAIL);
    unchanged(object, sizeof object); unchanged(data, sizeof data);
}
static void offline(void) {
    assert(h_ldn_Init() == RES_FAIL && !h_ldn_State()); h_bt_InitBle();
    Result (*bind[])(void *) = {h_bt_EvScan, h_bt_EvConn, h_bt_EvDisc, h_bt_EvMtu, h_bt_EvGatt};
    _Alignas(16) uint8_t storage[6][0x100] = {{0}};
    for (int repeat = 0; repeat < 32; repeat++) {
        h_ldn_Attach(storage[0]);
        for (int i = 0; i < 5; i++) assert(!bind[i](storage[i + 1]));
        for (int i = 0; i < 6; i++) {
            sdk_event *ev = (sdk_event *)storage[i];
            uint64_t r[32] = {0, (uintptr_t)&ev->read, 1, 0};
            assert(ev->write && ev->read && svc_thread(0x18, r) && r[0] == 0xea01);
            r[0] = ev->write; assert(svc_thread(0x16, r) && !r[0]);
            r[0] = ev->read; assert(svc_thread(0x16, r) && !r[0]);
        }
        int handles, live, objects; thr_counts(&handles, &live, &objects);
        assert(handles == 1 && live == 1 && objects == 1);
    }
    uint8_t out[64]; memset(out, 0xa5, sizeof out); int count = 99;
    assert(h_ldn_Scan(out, &count, 4, NULL, 0) == RES_FAIL && !count);
    count = 99; assert(h_ldn_ScanP(out, &count, 4, NULL, 0) == RES_FAIL && !count);
    assert(!h_bt_ScanRes(out, 4) && !h_bt_ConnList(out, 4));
    assert(h_bt_ScanPrm(out, 0) == RES_FAIL && h_bt_ScanStart(out) == RES_FAIL);
    assert(!h_bt_ScanStop() && h_bt_Connect(out) == RES_FAIL);
    h_bcat_Init(); assert(h_bcat_Mount() == RES_FAIL); h_bcat_Unmount();
    assert(h_web_Lobby(out, out) == RES_FAIL && h_web_Page(out, out) == RES_FAIL);
    assert(!h_prepo_Init() && !h_prepo_Save(out) && !h_prepo_SaveU(out, out));
    assert(!h_prepo_AddSess(out, "session") && !h_prepo_AddNet(out, "a", "b", "c") && !h_prepo_Tx());
    unchanged(out, sizeof out); h_ldn_Fini();
}
int main(void) {
    assert(svc_thread_main_init());
    requests(); waits(); sockets(); ssl(); offline();
    puts("network: offline results, TLS, request concurrency, wait delegation, guarded outputs and 192 event lifetimes passed");
    return 0;
}
