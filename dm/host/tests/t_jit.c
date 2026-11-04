// The real nn::jit API, loader and code mappings, with a synthetic NRO plugin.
// Plugin callbacks are compiled host functions; generated code is actual A64.
#include "../loader.c"
#include "../hle_jit.c"
#include <assert.h>
#include <stdatomic.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

struct dm_flags g_flag;
uint64_t now_ns(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void gate_register(uint64_t a, const char *k, int n, uint64_t o) { (void)a; (void)k; (void)n; (void)o; }
void hlecount_seal(void) {}
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
uint64_t resolve_import(module_t *m, const char *name) { (void)m; (void)name; abort(); }
int hle_implements(const char *name) { (void)name; return 0; }
int svc_nro_deactivate(module_t *m) { (void)m; abort(); } // JIT plugins never enter the game activation list.
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }

enum { REGION = 0x200000, WORK_SIZE = 3 * REGION, BAD_RANGE = 999 };
static struct {
    Elf64_Sym symbols[8];
    char names[256];
    Elf64_Dyn dynamic[1];
    uint64_t initial, bss;
} plugin_image;
static uint8_t raw_nro[128];
static int step, inits, finis;
static jit_config active_cfg;
static atomic_int requests;

static void resident(uintptr_t address, int expected) {
#ifdef __APPLE__
    mach_vm_address_t at = address;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    kern_return_t result = mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info, &count, &object);
    if (result == KERN_SUCCESS) {
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        assert((at <= address && address - at < size) == expected);
    } else assert(result == KERN_INVALID_ADDRESS && !expected);
#else
    unsigned char status;
    errno = 0;
    int result = mincore((void *)address, (size_t)sysconf(_SC_PAGESIZE), &status);
    if (expected) assert(result == 0);
    else assert(result == -1 && errno == ENOMEM);
#endif
}
static void plugin_init(void) {
    assert(step == 0); step++;
    assert(plugin_image.initial == 123 && plugin_image.bss == 0);
    plugin_image.initial = 456; plugin_image.bss = 789; inits++;
}
static uint64_t plugin_version(void) { assert(step == 1); step++; return 1; }
static void plugin_resolve(void *resolver) {
    assert(step == 2); step++;
    void *(*resolve)(const char *) = resolver;
    assert(resolve("memcpy") == (void *)memcpy);
    assert(resolve("memmove") == (void *)memmove);
    assert(resolve("memset") == (void *)memset);
}
static void plugin_diagnostics(uint32_t enabled, void **resolver) {
    assert(step == 3 && !enabled && *resolver == (void *)jit_resolve); step++;
}
static void plugin_configure(uint32_t *flags) { assert(step == 4 && !*flags); step++; }
static void plugin_prepared(jit_config *cfg) {
    assert(step == 5); step++;
    assert(cfg->user_rx.size == REGION && cfg->sys_rx.size == REGION);
    assert(cfg->user_ro.size == REGION && cfg->sys_ro.size == REGION);
    assert(cfg->tmem.size == REGION);
    active_cfg = *cfg;
}
static void plugin_fini(void) {
    assert(step == 6 && !atomic_load(&requests)); step = 0; finis++;
    resident(active_cfg.user_rx.offset, 1);
    resident(active_cfg.sys_rx.offset, 1);
    resident(active_cfg.user_ro.offset, 1);
    assert(plugin_image.initial == 456 && plugin_image.bss == 789);
}
static void enter_request(void) {
    assert(atomic_fetch_add(&requests, 1) == 0);
    for (int i = 0; i < 4; i++) sched_yield();
}
static void leave_request(void) { assert(atomic_fetch_sub(&requests, 1) == 1); }
static uint64_t plugin_generate(int *ret, jit_range *code_out, jit_range *data_out,
        jit_config *cfg, uint64_t cmd, const void *in, size_t in_size,
        const jit_range *code_in, const jit_range *data_in,
        const void *data, size_t data_size, void *out, size_t out_size) {
    enter_request();
    assert(in_size == 4 && out_size == 4 && data_size == 3);
    assert(!memcmp(data, "abc", 3));
    for (int i = 3; i < 32; i++) assert(!((const uint8_t *)data)[i]);
    if (cmd == BAD_RANGE) {
        *code_out = (jit_range){REGION - 4, 8}; *ret = 0;
    } else if (cmd == 7) {
        *ret = 42; // The API must propagate the plugin result, without a wrapper failure.
    } else {
        uint32_t value; memcpy(&value, in, 4); assert(value < 65536);
        assert(code_in->size >= 8 && range_ok(code_in, REGION));
        assert(data_in->size >= 4 && range_ok(data_in, REGION));
        uint32_t code[] = {0x52800000u | value << 5, 0xd65f03c0u};
        memcpy((void *)(cfg->sys_rx.offset + code_in->offset), code, sizeof code);
        memcpy((void *)(cfg->sys_ro.offset + data_in->offset), &value, 4);
        memcpy(out, &value, 4);
        *code_out = (jit_range){code_in->offset, sizeof code};
        *data_out = (jit_range){data_in->offset, 4}; *ret = 0;
    }
    leave_request(); return 0;
}
static uint64_t plugin_control(int *ret, jit_config *cfg, uint64_t cmd,
        const void *in, size_t in_size, void *out, size_t out_size) {
    enter_request();
    assert(cmd == 3 && in_size == 4 && out_size == 4);
    assert(cfg->tmem.offset == active_cfg.tmem.offset);
    memcpy(out, in, 4); *ret = 17;
    leave_request(); return 0;
}
static void setup_plugin(void) {
    allocate_modules(1, NULL); g_nmod = 1;
    module_t *m = g_mod;
    m->base = (uint8_t *)&plugin_image; m->size = sizeof plugin_image;
    strcpy(m->name, "jit-test"); m->nro = 1;
    m->symtab = plugin_image.symbols; m->strtab = plugin_image.names;
    m->dyn = plugin_image.dynamic; m->nsym = 8;
    m->data_off = (uint8_t *)&plugin_image.initial - m->base;
    m->pristine_len = sizeof plugin_image.initial;
    m->key_page_off = sizeof plugin_image;
    plugin_image.initial = 123;
    const struct { const char *name; void *fn; } exports[] = {
        {"nnjitpluginGetVersion", plugin_version}, {"nnjitpluginResolveBasicSymbols", plugin_resolve},
        {"nnjitpluginSetupDiagnostics", plugin_diagnostics}, {"nnjitpluginConfigure", plugin_configure},
        {"nnjitpluginOnPrepared", plugin_prepared}, {"nnjitpluginGenerateCode", plugin_generate},
        {"nnjitpluginControl", plugin_control},
    };
    unsigned offset = 1;
    for (unsigned i = 0; i < sizeof exports / sizeof *exports; i++) {
        strcpy(plugin_image.names + offset, exports[i].name);
        plugin_image.symbols[i + 1] = (Elf64_Sym){.st_name = offset, .st_info = 1 << 4,
            .st_shndx = 1, .st_value = (uintptr_t)exports[i].fn - (uintptr_t)m->base};
        offset += (unsigned)strlen(exports[i].name) + 1;
    }
    assert(offset <= sizeof plugin_image.names); build_symhash(m);
    m->init = (uintptr_t)plugin_init - (uintptr_t)m->base;
    m->fini = (uintptr_t)plugin_fini - (uintptr_t)m->base;
    memcpy(raw_nro + 0x10, "NRO0", 4); *(uint32_t *)(raw_nro + 0x18) = sizeof raw_nro;
    m->nro_size = sizeof raw_nro; CC_SHA256(raw_nro, sizeof raw_nro, m->sha256);
}
static void generate(jit_env *env, unsigned offset, uint32_t value) {
    jit_range ci = {offset, 8}, di = {offset, 4}, co = {0}, dout = {0};
    uint32_t out = 0;
    assert(h_jit_Generate(&co, &dout, env, 1, &value, 4, &ci, &di, "abc", 3, &out, 4) == 0);
    assert(co.offset == offset && co.size == 8 && dout.offset == offset && dout.size == 4);
    assert(out == value && *(uint32_t *)(env->ro + offset) == value);
}

// Keep the executor alive across replacements, so it can hold old instructions
// in its I-cache. The writer waits for consumption before replacing that slot.
typedef struct {
    jit_env *env;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t pending;
} mailbox;
static void *executor(void *arg) {
    mailbox *box = arg;
    assert(!pthread_mutex_lock(&box->mu));
    for (unsigned i = 0; i < 64; i++) {
        while (!box->pending) assert(!pthread_cond_wait(&box->cv, &box->mu));
        int (*run)(void) = (void *)(box->env->rx + 0x100);
        assert(run() == (int)box->pending);
        box->pending = 0; assert(!pthread_cond_signal(&box->cv));
    }
    assert(!pthread_mutex_unlock(&box->mu)); return NULL;
}
static void replace_on_existing_thread(jit_env *env) {
    mailbox box = {.env = env, .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
    pthread_t t; assert(!pthread_create(&t, NULL, executor, &box));
    assert(!pthread_mutex_lock(&box.mu));
    for (uint32_t i = 1; i <= 64; i++) {
        generate(env, 0x100, i);
        box.pending = i; assert(!pthread_cond_signal(&box.cv));
        while (box.pending) assert(!pthread_cond_wait(&box.cv, &box.mu));
    }
    assert(!pthread_mutex_unlock(&box.mu)); assert(!pthread_join(t, NULL));
    assert(!pthread_mutex_destroy(&box.mu)); assert(!pthread_cond_destroy(&box.cv));
}
typedef struct { jit_env *env; unsigned slot; } producer;
static void *concurrent_requests(void *arg) {
    producer *p = arg;
    unsigned offset = p->slot * 0x1000;
    for (uint32_t i = 1; i <= 128; i++) {
        generate(p->env, offset, i);
        assert(((int (*)(void))(p->env->rx + offset))() == (int)i);
        uint32_t out = 0;
        assert(h_jit_Control(p->env, 3, &i, 4, &out, 4) == 17 && out == i);
    }
    return NULL;
}
int main(void) {
    alarm(30); unsetenv("DM_JIT_SERVE"); unsetenv("DM_JIT_CAPTURE");
    setup_plugin();
    void *work; assert(!posix_memalign(&work, REGION, WORK_SIZE));
    memset(work, 0x5a, WORK_SIZE);
    uint64_t cfg[] = {REGION, REGION};
    uint64_t plug[] = {0, 0, (uintptr_t)raw_nro, sizeof raw_nro, REGION};
    for (int cycle = 0; cycle < 8; cycle++) {
        h_jit_Initialize(); void *handle = NULL;
        assert(!h_jit_Create(&handle, cfg, plug, work, WORK_SIZE) && handle);
        jit_env *env = handle; jit_info info = h_jit_Info(env);
        assert(info.rx == env->rx && info.rx_size == REGION && info.ro == env->ro && info.ro_size == REGION);
        assert(g_mod->loaded && !g_nloaded && !guest_sym("nnjitpluginGenerateCode"));
        assert(active_cfg.tmem.offset == (uintptr_t)work + 2 * REGION);
#ifdef __ANDROID__
        assert(active_cfg.sys_rx.offset != active_cfg.user_rx.offset);
#endif
        pthread_t threads[2]; producer producers[] = {{env, 1}, {env, 2}};
        for (int i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, concurrent_requests, &producers[i]));
        for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
        replace_on_existing_thread(env);
        jit_range ci = {0x100, 8}, di = {0x100, 4}, co = {1, 1}, dout = {1, 1};
        uint32_t value = 2, out = 0;
        assert(h_jit_Generate(&co, &dout, env, 7, &value, 4, &ci, &di, "abc", 3, &out, 4) == 42);
        assert(!co.offset && !co.size && !dout.offset && !dout.size);
        if (!cycle) {
            pid_t pid = fork(); assert(pid >= 0);
            if (!pid) {
                struct rlimit limit = {0, 0}; setrlimit(RLIMIT_CORE, &limit);
                h_jit_Generate(&co, &dout, env, BAD_RANGE, &value, 4, &ci, &di, "abc", 3, &out, 4);
                _exit(0);
            }
            int status; assert(waitpid(pid, &status, 0) == pid);
            assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        }
        uintptr_t rx = env->rx, rw = (uintptr_t)env->code.rw, ro = env->ro;
        // Guest joins users before destroying its handle; concurrent use after
        // Destroy is not a legal API lifetime and is deliberately not promised.
        h_jit_Destroy(env); h_jit_Finalize();
        resident(rx, 0); resident(rw, 0); resident(ro, 0);
        assert(!g_mod->loaded && !g_jit_env && inits == cycle + 1 && finis == inits);
        assert(((uint8_t *)work)[0] == 0x5a && ((uint8_t *)work)[WORK_SIZE - 1] == 0x5a);
    }
    h_jit_Destroy(NULL);
    free(work); free(g_mod->symh); free(g_mod->pristine); free(g_mod); free(g_loaded);
    puts("JIT guest API, cross-thread code replacement, serialized requests and repeated lifetimes: OK");
}
