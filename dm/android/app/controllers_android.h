// Called under g_pmu. A framework device ID lasts until unplug; Android's
// persistent descriptor lets the shared player-slot policy recognize a reconnect.
#include "pad_slots.h"

enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_HX, AX_HY, AX_LT, AX_RT, AX_BRAKE, AX_GAS, AX_COUNT };
typedef struct { uint64_t btn; float ax[AX_COUNT]; } android_pad;
static pad_slots g_slots;
static android_pad g_pads[DM_PLAYERS], g_touch, g_keys;
static struct {
    uint64_t last, held, pressed[16], combo_since;
} g_timing[DM_PLAYERS];
static int g_tick_pads, g_focused, g_resumed;

static const void *device_owner(int32_t id) {
    return id < 0 ? NULL : (const void *)((uintptr_t)(uint32_t)id + 1);
}
static android_pad *event_pad(int32_t id, int controller) {
    int p = pad_slot_of(&g_slots, device_owner(id));
    if (p >= 0) return &g_pads[p];
    // Keyboard and shell-injected buttons belong to player one. An unassigned
    // gamepad (e.g. a ninth controller) must never feed that player.
    return id < 0 || !controller ? &g_keys : NULL;
}
static uint64_t pad_buttons(const android_pad *pad) {
    const float *a = pad->ax;
    uint64_t b = pad->btn;
    if (a[AX_HX] < -0.5f) b |= DM_BTN_LEFT;
    if (a[AX_HX] > 0.5f) b |= DM_BTN_RIGHT;
    if (a[AX_HY] < -0.5f) b |= DM_BTN_UP;
    if (a[AX_HY] > 0.5f) b |= DM_BTN_DOWN;
    if (a[AX_LT] > 0.5f || a[AX_BRAKE] > 0.5f) b |= DM_BTN_ZL;
    if (a[AX_RT] > 0.5f || a[AX_GAS] > 0.5f) b |= DM_BTN_ZR;
    return b;
}
static void combine_pad(const android_pad *pad, uint64_t *buttons, float axes[4]) {
    *buttons |= pad_buttons(pad);
    for (int i = 0; i < 4; i += 2) {
        if (hypotf(pad->ax[i], pad->ax[i+1]) > hypotf(axes[i], axes[i+1])) {
            axes[i] = pad->ax[i]; axes[i+1] = pad->ax[i+1];
        }
    }
}
static void pads_push_(void) {
    uint64_t now = now_ns();
    g_tick_pads = 0;
    for (int p = 0; p < DM_PLAYERS; p++) {
        if (p && !g_slots.owner[p]) continue;
        uint64_t btn = pad_buttons(&g_pads[p]);
        float axes[4]; memcpy(axes, g_pads[p].ax, sizeof axes);
        if (!p) { combine_pad(&g_keys, &btn, axes); combine_pad(&g_touch, &btn, axes); }
        // Keep the fork's 50 ms minimum tap per player. The guest can sample
        // many times per frame before the read it actually uses.
        typeof(g_timing[0]) *timing = &g_timing[p];
        for (int i = 0; i < 16; i++) {
            uint64_t bit = 1ull << i;
            if ((btn & bit) && !(timing->last & bit)) timing->pressed[i] = now;
            if (!(btn & bit) && (timing->last & bit)) timing->held |= bit;
            if ((btn & bit) || now - timing->pressed[i] >= 50000000) timing->held &= ~bit;
        }
        timing->last = btn; btn |= timing->held;
        int32_t stick[4];
        for (int i = 0; i < 4; i++) {
            float v = fabsf(axes[i]) < .12f ? 0 : fmaxf(-1, fminf(1, axes[i]));
            stick[i] = (int32_t)(v * DM_STICK_MAX);
        }
        dm_pad_set_player(p, btn, stick[0], -stick[1], stick[2], -stick[3]);
        if ((btn & (DM_BTN_MINUS | DM_BTN_PLUS)) != (DM_BTN_MINUS | DM_BTN_PLUS)) timing->combo_since = 0;
        else if (!timing->combo_since) timing->combo_since = now;
        else if (now - timing->combo_since > 2000000000) atomic_store(&g_menu_request, 1);
        if (timing->held || timing->combo_since) g_tick_pads = 1;
    }
}

static void cancel_pads(void) {
    memset(g_pads, 0, sizeof g_pads); memset(&g_keys, 0, sizeof g_keys); memset(&g_touch, 0, sizeof g_touch);
    memset(g_timing, 0, sizeof g_timing); g_tick_pads = 0;
    for (int p = 0; p < DM_PLAYERS; p++) dm_pad_cancel_player(p);
}

static jboolean pad_attach(JNIEnv *env, jclass cls, jint id, jstring descriptor) {
    (void)cls;
    const char *name = (*env)->GetStringUTFChars(env, descriptor, NULL);
    if (!name) return JNI_FALSE;
    pthread_mutex_lock(&g_pmu);
    int p = pad_slot_take(&g_slots, device_owner(id), name);
    if (p >= 0) {
        dm_pad_players(pad_slot_mask(&g_slots));
        LOG("front end: controller %d connected to player %d", id, p + 1);
    }
    pthread_mutex_unlock(&g_pmu);
    (*env)->ReleaseStringUTFChars(env, descriptor, name);
    return p >= 0;
}
static void pad_detach(JNIEnv *env, jclass cls, jint id) {
    (void)env; (void)cls;
    pthread_mutex_lock(&g_pmu);
    int p = pad_slot_free(&g_slots, device_owner(id));
    if (p >= 0) {
        LOG("front end: controller %d disconnected from player %d", id, p + 1);
        memset(&g_pads[p], 0, sizeof g_pads[p]); memset(&g_timing[p], 0, sizeof g_timing[p]);
        dm_pad_cancel_player(p);
        dm_pad_players(pad_slot_mask(&g_slots));
        pads_push_();
    }
    pthread_mutex_unlock(&g_pmu);
}
