// The HID sender only publishes current motor levels. The activity looper owns the
// vibrator routes and applies the latest levels, without Binder calls under the HID lock.
#include "hle_haptics.h"
#include <sys/eventfd.h>

static int rumble_fd = -1;
static pthread_mutex_t rumble_lock = PTHREAD_MUTEX_INITIALIZER;
static dm_haptic_state rumble_pending[DM_PLAYERS];
static unsigned rumble_dirty;
static int rumble_controller[DM_PLAYERS];
static float rumble_gain, rumble_floor;
static jmethodID rumble_apply;

static void rumble_enqueue(dm_haptic_state state) {
    pthread_mutex_lock(&rumble_lock);
    int wake = !rumble_dirty;
    rumble_pending[state.player] = state;
    rumble_dirty |= 1u << state.player;
    pthread_mutex_unlock(&rumble_lock);
    if (wake) while (eventfd_write(rumble_fd, 1) < 0 && errno == EINTR) {}
}
static int rumble_drain(int fd, int events, void *arg) {
    (void)events;
    ANativeActivity *a = arg;
    eventfd_t count;
    while (eventfd_read(fd, &count) < 0 && errno == EINTR) {}
    dm_haptic_state states[DM_PLAYERS];
    pthread_mutex_lock(&rumble_lock);
    unsigned dirty = rumble_dirty;
    memcpy(states, rumble_pending, sizeof states);
    rumble_dirty = 0;
    pthread_mutex_unlock(&rumble_lock);
    JNIEnv *env = a->env;
    for (int p = 0; p < DM_PLAYERS; p++) {
        if (!(dirty & (1u << p)) || states[p].epoch != dm_haptics_epoch(p)) continue;
        const dm_vibration *v = states[p].motor;
        dm_haptic_level levels[] = {dm_haptics_level(v, 1, rumble_gain), dm_haptics_level(v+1, 1, rumble_gain),
                                    dm_haptics_level(v, 2, rumble_gain)};
        if (rumble_controller[p])
            for (int i = 0; i < 3; i++)
                levels[i].intensity = fmaxf(0, levels[i].intensity - rumble_floor) / (1 - rumble_floor);
        (*env)->CallVoidMethod(env, a->clazz, rumble_apply, p, levels[0].intensity, levels[0].sharpness,
            levels[1].intensity, levels[1].sharpness, levels[2].intensity, levels[2].sharpness);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env);
            LOG("[RED] haptics: Android failed to apply player %d vibration", p + 1);
        }
    }
    return 1;
}
static void rumble_route(JNIEnv *env, jclass cls, jint player, jboolean available, jboolean controller) {
    (void)env; (void)cls;
    if ((unsigned)player >= DM_PLAYERS) return;
    rumble_controller[player] = controller;
    dm_haptics_route(player, available);
}
static float rumble_setting(const char *name, float def, float max) {
    const char *s = getenv(name);
    if (!s) return def;
    float value = strtof(s, NULL);
    return isfinite(value) ? fmaxf(0, fminf(max, value)) : def;
}
static void rumble_close(void) {
    if (rumble_fd < 0) return;
    dm_haptics_init(NULL, 0); // waits for any sender to finish its wakeup before closing the fd
    ALooper_removeFd(ALooper_forThread(), rumble_fd);
    close(rumble_fd); rumble_fd = -1; rumble_dirty = 0;
}
static void rumble_open(ANativeActivity *a) {
    rumble_close();
    JNIEnv *env = a->env;
    jclass cls = (*env)->GetObjectClass(env, a->clazz);
    rumble_apply = (*env)->GetMethodID(env, cls, "applyHaptics", "(IFFFFFF)V");
    (*env)->DeleteLocalRef(env, cls);
    if (!rumble_apply) die("Android haptics callback missing");
    rumble_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (rumble_fd < 0 || ALooper_addFd(ALooper_forThread(), rumble_fd, ALOOPER_POLL_CALLBACK,
            ALOOPER_EVENT_INPUT, rumble_drain, a) != 1) {
        if (rumble_fd >= 0) close(rumble_fd);
        rumble_fd = -1;
        LOG("[RED] haptics: Android looper registration failed");
        return;
    }
    rumble_gain = rumble_setting("DM_RUMBLE_GAIN", 1, 3);
    rumble_floor = rumble_setting("DM_RUMBLE_MIN", 0, .9f);
    const char *enable = getenv("DM_RUMBLE");
    dm_haptics_init(rumble_enqueue, !enable || atoi(enable));
    dm_haptics_suspend(DM_HAPTICS_INACTIVE | DM_HAPTICS_INTERRUPTED, 1);
}
