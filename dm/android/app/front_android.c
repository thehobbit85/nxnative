// Android app front end (front.h; Mac: front_mac.m, iOS: dm/ios): the native half of the app, a NativeActivity in a process of its own (":game"). The start-up menu
// (MenuActivity.java, in the app's main process) writes the player's choices to <files>/settings.env and starts this activity; here the environment is set from it, the
// host runs on a big-stack thread, the window goes to the Vulkan backend (front_native_window), and controllers -- the console's own and any other, an Xbox one over
// USB or Bluetooth -- reach the game through dm_pad_set, by position as on the Mac and iOS (bottom B, right A, top X, left Y).
// Phones without a controller get on-screen controls (front_overlay, drawn by the Vulkan backend over the picture): they show at the first touch and hide again
// when a controller is used. Holding − and + together for two seconds goes back to the menu: the game process ends (progress the game has saved is kept) and the menu opens with the options shown.
// Game files live in the app's external folder (<external>/romfs, dm, vk, font), the saves in its internal one (<files>/save); the log goes to <external>/log.txt.
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include "android_contract.h"
#include "program_boot.h"
#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>
#include <math.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

int dm_main(int argc, char **argv);   // dm/host/main.c, compiled with -Dmain=dm_main
void dm_focus_set(int st);
void dm_audio_suspend(int on);
void dm_gfx_suspend(int on);
void dm_host_shutdown(const char *why);

static ANativeActivity *g_act;
static pthread_mutex_t g_wmu = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *g_win;
static _Atomic int g_win_gen;   // bumped whenever the window changes: the backend makes a new swapchain
// The window the backend presents to (NULL while there is none), and its generation; the backend holds it with front_window_lock while it uses it
void *front_native_window(int *gen) { if (gen) *gen = atomic_load(&g_win_gen); return g_win; }
void front_window_lock(int on) { if (on) pthread_mutex_lock(&g_wmu); else pthread_mutex_unlock(&g_wmu); }
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 0; }
#include "keyboard_android.h"
// Shared host_preload initializes cheats after resolving the modules' build IDs.
void front_run(void (*guest)(void)) { guest(); }

int front_display_pixels(int *w, int *h) {
    if (!g_act) return 0;
    JNIEnv *env = NULL; int attached = 0, ok = 0;
    if ((*g_act->vm)->GetEnv(g_act->vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*g_act->vm)->AttachCurrentThread(g_act->vm, &env, NULL) != JNI_OK) return 0;
        attached = 1;
    }
    if (!env) return 0;
    jclass cls = (*env)->GetObjectClass(env, g_act->clazz);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "displayPixels", "()[I") : NULL;
    jintArray size = method ? (*env)->CallObjectMethod(env, g_act->clazz, method) : NULL;
    if (!(*env)->ExceptionCheck(env) && size && (*env)->GetArrayLength(env, size) == 2) {
        jint pixels[2]; (*env)->GetIntArrayRegion(env, size, 0, 2, pixels);
        if (!(*env)->ExceptionCheck(env) && pixels[0] > 0 && pixels[1] > 0) { *w = pixels[0]; *h = pixels[1]; ok = 1; }
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (size) (*env)->DeleteLocalRef(env, size);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (attached) (*g_act->vm)->DetachCurrentThread(g_act->vm);
    return ok;
}

static uint64_t key_bit(int32_t k) {
    switch (k) {
        case AKEYCODE_BUTTON_B: return DM_BTN_A;  case AKEYCODE_BUTTON_A: return DM_BTN_B;   // by position: right = A, bottom = B
        case AKEYCODE_BUTTON_Y: return DM_BTN_X;  case AKEYCODE_BUTTON_X: return DM_BTN_Y;   // top = X, left = Y
        case AKEYCODE_BUTTON_L1: return DM_BTN_L; case AKEYCODE_BUTTON_R1: return DM_BTN_R;
        case AKEYCODE_BUTTON_L2: return DM_BTN_ZL; case AKEYCODE_BUTTON_R2: return DM_BTN_ZR;
        case AKEYCODE_BUTTON_THUMBL: return DM_BTN_LS; case AKEYCODE_BUTTON_THUMBR: return DM_BTN_RS;
        case AKEYCODE_BUTTON_START: return DM_BTN_PLUS; case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BACK: return DM_BTN_MINUS;
        case AKEYCODE_DPAD_LEFT: return DM_BTN_LEFT; case AKEYCODE_DPAD_UP: return DM_BTN_UP; case AKEYCODE_DPAD_RIGHT: return DM_BTN_RIGHT; case AKEYCODE_DPAD_DOWN: return DM_BTN_DOWN;
    }
    return 0;
}
static _Atomic int g_menu_request;
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
#include "controllers_android.h"
#include "motion_android.h"
#include "haptics_android.h"
#include "controller_prompt_android.h"

static void back_to_menu(void);
static void *latch_thread(void *a) {
    (void)a;
    for (;;) {
        usleep(4000); pthread_mutex_lock(&g_pmu);
        if (g_tick_pads) pads_push_();
        pthread_mutex_unlock(&g_pmu);
        if (atomic_load(&g_menu_request)) back_to_menu();
    }
    return NULL;
}
// ---------- On-screen controls ----------
// Laid out from the window's height h: the D-pad bottom left, A B X Y bottom right (by position, as on the Switch), L / ZL and R / ZR in the top corners, − and + top centre
typedef struct { float x, y, w, h; int on; char label[3]; } dm_ovl;
enum { NVB = 15 };
static struct { const char *label; uint64_t bit; float cx, cy; } g_vb[NVB];
static _Atomic int g_ovl_show; static _Atomic uint64_t g_ovl_on; static float g_vw, g_vh, g_vr, g_dx, g_dy;
static void vb_layout(void) {
    ANativeWindow *w = front_native_window(NULL); if (!w) return;
    float W_ = (float)ANativeWindow_getWidth(w), H = (float)ANativeWindow_getHeight(w);
    if (W_ == g_vw && H == g_vh) return;
    g_vw = W_; g_vh = H; g_vr = 0.065f * H; float o = 0.12f * H, fx = W_ - 0.24f * H, fy = H - 0.25f * H;
    g_dx = 0.24f * H; g_dy = H - 0.25f * H;
    int i = 0;
    #define VB(l, b, x, y) g_vb[i++] = (typeof(g_vb[0])){l, b, x, y}
    VB("A", DM_BTN_A, fx + o, fy); VB("B", DM_BTN_B, fx, fy + o); VB("X", DM_BTN_X, fx, fy - o); VB("Y", DM_BTN_Y, fx - o, fy);
    VB("L", DM_BTN_L, 0.11f * H, 0.11f * H); VB("ZL", DM_BTN_ZL, 0.11f * H, 0.28f * H); VB("R", DM_BTN_R, W_ - 0.11f * H, 0.11f * H); VB("ZR", DM_BTN_ZR, W_ - 0.11f * H, 0.28f * H);
    VB("-", DM_BTN_MINUS, W_ / 2 - 0.1f * H, 0.08f * H); VB("+", DM_BTN_PLUS, W_ / 2 + 0.1f * H, 0.08f * H);
    VB("", DM_BTN_UP, g_dx, g_dy - o); VB("", DM_BTN_DOWN, g_dx, g_dy + o); VB("", DM_BTN_LEFT, g_dx - o, g_dy); VB("", DM_BTN_RIGHT, g_dx + o, g_dy);
    VB("", 0, g_dx, g_dy);
    #undef VB
}
// What the backend draws over the picture: one box per control, in window pixels (0 = hidden)
int front_overlay(dm_ovl *out, int max) {
    if (!atomic_load(&g_ovl_show)) return 0;
    vb_layout(); uint64_t on = atomic_load(&g_ovl_on); int n = 0;
    for (int i = 0; i < NVB && n < max; i++) {
        dm_ovl *b = &out[n++]; float r = g_vb[i].bit ? g_vr : g_vr * 0.6f;
        *b = (dm_ovl){g_vb[i].cx - r, g_vb[i].cy - r, 2 * r, 2 * r, g_vb[i].bit && (on & g_vb[i].bit) != 0, ""};
        snprintf(b->label, sizeof b->label, "%s", g_vb[i].label);
    }
    return n;
}
// Every finger on the screen: the buttons under it; on the D-pad its direction (eight ways) and the left stick, from the finger's offset to the pad's centre
static void touch_event(AInputEvent *e) {
    vb_layout();
    int32_t act = AMotionEvent_getAction(e), kind = act & AMOTION_EVENT_ACTION_MASK, up_idx = -1;
    if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL) up_idx = -2;
    else if (kind == AMOTION_EVENT_ACTION_POINTER_UP) up_idx = (act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    uint64_t btn = 0; float sx = 0, sy = 0;
    if (up_idx != -2) for (size_t p = 0; p < AMotionEvent_getPointerCount(e); p++) {
        if ((int32_t)p == up_idx) continue;
        float x = AMotionEvent_getX(e, p), y = AMotionEvent_getY(e, p), ddx = x - g_dx, ddy = y - g_dy;
        if (fabsf(ddx) < 0.24f * g_vh && fabsf(ddy) < 0.24f * g_vh) {
            float m = hypotf(ddx, ddy), dz = 0.03f * g_vh;
            if (m > dz) { if (ddx < -0.4f * m) btn |= DM_BTN_LEFT; if (ddx > 0.4f * m) btn |= DM_BTN_RIGHT; if (ddy < -0.4f * m) btn |= DM_BTN_UP; if (ddy > 0.4f * m) btn |= DM_BTN_DOWN;
                          float k = fminf(1.f, m / (0.12f * g_vh)); sx = ddx / m * k; sy = ddy / m * k; }
            continue;
        }
        for (int i = 0; i < 10; i++) if (fabsf(x - g_vb[i].cx) < 1.3f * g_vr && fabsf(y - g_vb[i].cy) < 1.3f * g_vr) btn |= g_vb[i].bit;
    }
    g_touch.btn = btn;
    g_touch.ax[AX_LX] = sx; g_touch.ax[AX_LY] = sy;
    atomic_store(&g_ovl_on, btn); atomic_store(&g_ovl_show, 1);
    pads_push_();
}
static int on_input_locked(AInputEvent *e) {
    int32_t type = AInputEvent_getType(e), src = AInputEvent_getSource(e);
    static int seen; if (!seen++) LOG("front end: first input event (type %d, source %#x)", type, src);
    if (!g_focused || !g_resumed) return 0;
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) { touch_event(e); return 1; }
    int controller = (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
    android_pad *pad = event_pad(AInputEvent_getDeviceId(e), controller);
    if (!pad) return 0;
    if (controller || (src & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD) atomic_store(&g_ovl_show, 0);   // a controller in use: the on-screen controls go
    if (type == AINPUT_EVENT_TYPE_KEY) {
        uint64_t b = key_bit(AKeyEvent_getKeyCode(e)); if (!b) return AKeyEvent_getKeyCode(e) == AKEYCODE_BACK;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) pad->btn |= b; else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) pad->btn &= ~b;
        pads_push_(); return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        static const int32_t axes[10] = {AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RZ, AMOTION_EVENT_AXIS_HAT_X, AMOTION_EVENT_AXIS_HAT_Y,
                                         AMOTION_EVENT_AXIS_LTRIGGER, AMOTION_EVENT_AXIS_RTRIGGER, AMOTION_EVENT_AXIS_BRAKE, AMOTION_EVENT_AXIS_GAS};
        for (int k = 0; k < 10; k++) pad->ax[k] = AMotionEvent_getAxisValue(e, axes[k], 0);
        pads_push_(); return 1;
    }
    return 0;
}
static int on_input(AInputEvent *e) {
    // Both touch layout and controller latching are read from other threads.
    // Window first, then pads; the present thread already holds the window lock.
    pthread_mutex_lock(&g_wmu); pthread_mutex_lock(&g_pmu);
    int handled = on_input_locked(e);
    pthread_mutex_unlock(&g_pmu); pthread_mutex_unlock(&g_wmu);
    if (atomic_load(&g_menu_request)) back_to_menu();
    return handled;
}
static AInputQueue *g_iq;
static int input_cb(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data; AInputEvent *e;
    while (g_iq && AInputQueue_getEvent(g_iq, &e) >= 0) {
        if (AInputQueue_preDispatchEvent(g_iq, e)) continue;
        AInputQueue_finishEvent(g_iq, e, on_input(e));
    }
    return 1;
}

#include "web_android.h"

// ---------- Back to the menu: a flag for the menu, then this process ends ----------
static char g_files[512];
static int g_program;
// The Java handoff activity belongs to the menu process. It observes this
// process's Binder death before it starts a fresh host; shared C globals never
// need an in-place reset. hle_oe has already closed its launch record here.
void front_program_handoff(int index, const char *name) {
    (void)index;
    JNIEnv *env = NULL; int attached = 0;
    if ((*g_act->vm)->GetEnv(g_act->vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*g_act->vm)->AttachCurrentThread(g_act->vm, &env, NULL) != JNI_OK) { LOG("[RED] program handoff: cannot attach guest thread to Java"); return; }
        attached = 1;
    }
    if (!env) { LOG("[RED] program handoff: no Java environment"); return; }
    jclass cls = (*env)->GetObjectClass(env, g_act->clazz);
    jmethodID method = (*env)->GetMethodID(env, cls, "programHandoff", "(ILjava/lang/String;)V");
    jstring title = name ? (*env)->NewStringUTF(env, name) : NULL;
    if (method) (*env)->CallVoidMethod(env, g_act->clazz, method, g_program, title);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env);
        LOG("[RED] program handoff: Android could not start the next-program activity");
    }
    if (title) (*env)->DeleteLocalRef(env, title);
    (*env)->DeleteLocalRef(env, cls);
    if (attached) (*g_act->vm)->DetachCurrentThread(g_act->vm);
}
static void back_to_menu(void) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    char p[600]; snprintf(p, sizeof p, "%s/show_menu", g_files); FILE *f = fopen(p, "w"); if (f) fclose(f);
    LOG("front end: − and + held: back to the menu");
    ANativeActivity_finish(g_act);
    dm_host_shutdown("back to the menu");
}

// ---------- Boot: environment from the menu's settings, log, then the host on its own thread ----------
static char *g_argv[4];
static void boot_env(const char *files, const char *ext, const char *assets, int handoff, int previous) {
    char p[1024];
    snprintf(p, sizeof p, "%s/log.txt", ext);
    char prev[1024]; snprintf(prev, sizeof prev, "%s/log.prev.txt", ext); rename(p, prev);
    if (freopen(p, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    snprintf(p, sizeof p, "%s/settings.env", files);   // KEY=VALUE lines from MenuActivity
    FILE *f = fopen(p, "r");
    if (f) { char ln[1024]; while (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; char *eq = strchr(ln, '='); if (!eq || ln[0] == '#') continue; *eq = 0; setenv(ln, eq + 1, 1); fprintf(stderr, "boot: %s=%s\n", ln, eq + 1); } fclose(f); }
    else fprintf(stderr, "boot: no settings.env, the defaults apply\n");
    snprintf(p, sizeof p, "%s/env.txt", ext);   // diagnostics: switches in <external>/env.txt (one KEY=VALUE per line) win
    if ((f = fopen(p, "r"))) { char ln[1024]; while (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; char *eq = strchr(ln, '='); if (!eq || ln[0] == '#') continue; *eq = 0; setenv(ln, eq + 1, 1); fprintf(stderr, "boot: env %s=%s\n", ln, eq + 1); } fclose(f); }
#ifdef DM_GAME_MULTI
    int have[sizeof dm_games / sizeof *dm_games];
    for (size_t i = 0; i < sizeof have / sizeof *have; i++) have[i] = dm_games[i].program;
#else
    const int have[] = {0};
#endif
    char root[1024];
    g_program = dm_android_program_boot(files, assets, handoff, previous, have, sizeof have / sizeof *have, root, sizeof root);
    if (g_program < 0) die("preparing Android program resources");
    snprintf(p, sizeof p, "%s/save", files); mkdir(p, 0755); setenv("DM_SAVE", p, 0);
    snprintf(p, sizeof p, "%s/temp", files); mkdir(p, 0755); setenv("DM_TEMP", p, 0);
    snprintf(p, sizeof p, "%s/vk-cache", files); mkdir(p, 0700); setenv("DM_VK_CACHE_DIR", p, 0);
    snprintf(p, sizeof p, "%s/cheats", files); mkdir(p, 0755); setenv("DM_CHEATS", p, 0);
    chdir(files);
    g_argv[0] = strdup("dmhost");
    if (asprintf(&g_argv[1], "%s/dm", root) < 0) die("allocating program module path");
}
static void *boot_thread(void *a) {
    (void)a;
    static char alt[65536]; stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    int rc = dm_main(2, g_argv);
    fprintf(stderr, "boot: dm_main returned %d\n", rc);
    return NULL;
}

// Immersive full screen: Android's navigation bar and gesture handle hidden (View.setSystemUiVisibility, on the UI thread)
static void immersive(ANativeActivity *a) {
    JNIEnv *env = a->env;
    jclass ac = (*env)->GetObjectClass(env, a->clazz);
    jobject win = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "getWindow", "()Landroid/view/Window;"));
    jobject dv = (*env)->CallObjectMethod(env, win, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, win), "getDecorView", "()Landroid/view/View;"));
    (*env)->CallVoidMethod(env, dv, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, dv), "setSystemUiVisibility", "(I)V"), 0x1706);   // IMMERSIVE_STICKY | LAYOUT_* | HIDE_NAVIGATION | FULLSCREEN
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

// ---------- Activity callbacks ----------
static void on_window_created(ANativeActivity *a, ANativeWindow *w) { (void)a; pthread_mutex_lock(&g_wmu); g_win = w; atomic_fetch_add(&g_win_gen, 1); pthread_mutex_unlock(&g_wmu); LOG("front end: window %dx%d", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w)); }
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; pthread_mutex_lock(&g_wmu); g_win = NULL; atomic_fetch_add(&g_win_gen, 1); pthread_mutex_unlock(&g_wmu); }
static void on_window_resized(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; atomic_fetch_add(&g_win_gen, 1); }
static void on_input_created(ANativeActivity *a, AInputQueue *q) { (void)a; g_iq = q; AInputQueue_attachLooper(q, ALooper_forThread(), 1, input_cb, NULL); }
static void on_input_destroyed(ANativeActivity *a, AInputQueue *q) { (void)a; AInputQueue_detachLooper(q); g_iq = NULL; }
static void on_resume(ANativeActivity *a) { (void)a; pthread_mutex_lock(&g_pmu); g_resumed = 1; pthread_mutex_unlock(&g_pmu); dm_haptics_suspend(DM_HAPTICS_INACTIVE, 0); dm_gfx_suspend(0); dm_focus_set(1); dm_audio_suspend(0); LOG("front end: resumed"); }
static void on_pause(ANativeActivity *a) {
    (void)a; dm_haptics_suspend(DM_HAPTICS_INACTIVE, 1); dm_audio_suspend(1); dm_focus_set(3); dm_gfx_suspend(1);
    pthread_mutex_lock(&g_pmu);
    g_resumed = 0; cancel_pads(); atomic_store(&g_ovl_on, 0);
    pthread_mutex_unlock(&g_pmu); LOG("front end: paused");
}
static void on_focus(ANativeActivity *a, int f) {
    pthread_mutex_lock(&g_pmu);
    g_focused = f;
    if (!f) { cancel_pads(); atomic_store(&g_ovl_on, 0); }
    pthread_mutex_unlock(&g_pmu);
    dm_haptics_suspend(DM_HAPTICS_INTERRUPTED, !f);
    if (f) immersive(a);
    if (f || !atomic_load(&g_keyboard_requests)) dm_focus_set(f ? 1 : 2);
}
static void on_destroy(ANativeActivity *a) {
    pthread_mutex_lock(&g_wmu); if (a == g_act) g_keyboard_live = 0; pthread_mutex_unlock(&g_wmu);
    rumble_close();
    dm_host_shutdown("activity destroyed");
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_n) {
    (void)saved; (void)saved_n;
    pthread_mutex_lock(&g_wmu); g_act = a; g_keyboard_live = 1; pthread_mutex_unlock(&g_wmu);
    web_register(a->env, a->clazz);
    a->callbacks->onNativeWindowCreated = on_window_created; a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onNativeWindowResized = on_window_resized;
    a->callbacks->onInputQueueCreated = on_input_created; a->callbacks->onInputQueueDestroyed = on_input_destroyed;
    a->callbacks->onResume = on_resume; a->callbacks->onPause = on_pause; a->callbacks->onWindowFocusChanged = on_focus; a->callbacks->onDestroy = on_destroy;
    ANativeActivity_setWindowFlags(a, 0x80 /* KEEP_SCREEN_ON */ | 0x400 /* FULLSCREEN */, 0);
    immersive(a);
    static int booted; if (booted++) { rumble_open(a); return; }   // the activity can be made again (rotation, a return): the host runs once per process
    snprintf(g_files, sizeof g_files, "%s", a->internalDataPath);
    JNIEnv *env = a->env;
    jclass cls = (*env)->GetObjectClass(env, a->clazz);
    jobject intent = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, cls, "getIntent", "()Landroid/content/Intent;"));
    jclass ic = (*env)->GetObjectClass(env, intent);
    jstring hk = (*env)->NewStringUTF(env, "handoff"), pk = (*env)->NewStringUTF(env, "previous");
    int handoff = (*env)->CallBooleanMethod(env, intent, (*env)->GetMethodID(env, ic, "getBooleanExtra", "(Ljava/lang/String;Z)Z"), hk, JNI_FALSE);
    int previous = (*env)->CallIntMethod(env, intent, (*env)->GetMethodID(env, ic, "getIntExtra", "(Ljava/lang/String;I)I"), pk, -1);
    jstring rk = (*env)->NewStringUTF(env, "gameRoot");
    jstring selected = (*env)->CallObjectMethod(env, intent, (*env)->GetMethodID(env, ic, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;"), rk);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); die("reading Android program launch context"); }
    (*env)->DeleteLocalRef(env, hk); (*env)->DeleteLocalRef(env, pk); (*env)->DeleteLocalRef(env, ic);
    (*env)->DeleteLocalRef(env, intent); (*env)->DeleteLocalRef(env, cls);
    const char *diagnostics = a->externalDataPath ? a->externalDataPath : a->internalDataPath;
    const char *assets = selected ? (*env)->GetStringUTFChars(env, selected, NULL) : diagnostics;
    if (!assets) die("reading Android game data location");
    boot_env(a->internalDataPath, diagnostics, assets, handoff, previous);
    if (selected) { (*env)->ReleaseStringUTFChars(env, selected, assets); (*env)->DeleteLocalRef(env, selected); }
    (*env)->DeleteLocalRef(env, rk);
    rumble_open(a);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16 << 20);
    pthread_t th; if (pthread_create(&th, &at, boot_thread, NULL)) die("front end: cannot start the host");
    pthread_attr_destroy(&at);
    pthread_detach(th);
    if (!pthread_create(&th, NULL, latch_thread, NULL)) pthread_detach(th);
}
