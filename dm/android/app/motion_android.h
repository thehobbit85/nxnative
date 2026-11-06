// The platform timestamps sensor events against elapsed realtime (including
// suspend); the host integrates on CLOCK_MONOTONIC. Take their offset once per
// subscription. All samples of a batch then retain their original spacing.
#include "motion_fusion.h"
static jlong motion_clock_offset(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    uint64_t mono = now_ns();
    struct timespec boot; clock_gettime(CLOCK_BOOTTIME, &boot);
    return (jlong)((uint64_t)boot.tv_sec * 1000000000 + boot.tv_nsec - mono);
}
static jint motion_modes(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    const char *m = getenv("DM_MOTION"), *phone = getenv("DM_PHONE_MOTION");
    if (m && !atoi(m)) return 0;
    return 2 | (!phone || !strcmp(phone, "auto") || atoi(phone) ? 1 : 0);
}
static void motion_sample(JNIEnv *env, jclass cls, jint device, jint rotation, jlong sample,
                          jfloat ax, jfloat ay, jfloat az, jfloat wx, jfloat wy, jfloat wz) {
    (void)env; (void)cls;
    if (sample <= 0) return;
    // Android reports specific force in m/s²; the guest wants gravity direction in G.
    float acc[] = {-ax / 9.80665f, -ay / 9.80665f, -az / 9.80665f}, rate[] = {wx, wy, wz};
    if (device == -1) {
        static const int orient[] = {1, 3, 2, 4}; // Surface.ROTATION_0/90/180/270
        if ((unsigned)rotation >= 4) return;
        float a[3], w[3];
        motion_phone_to_switch(orient[rotation], acc, a);
        motion_phone_to_switch(orient[rotation], rate, w);
        dm_motion_set_sample(0, a, w, sample, 1);
    } else {
        pthread_mutex_lock(&g_pmu);
        int p = pad_slot_of(&g_slots, device_owner(device));
        if (p >= 0) dm_motion_set_sample(p, acc, rate, sample, 0);
        pthread_mutex_unlock(&g_pmu);
    }
}
static void motion_clear(JNIEnv *env, jclass cls, jint device) {
    (void)env; (void)cls;
    if (device == -1) { dm_motion_clear_switch(); return; }
    pthread_mutex_lock(&g_pmu);
    int p = pad_slot_of(&g_slots, device_owner(device));
    if (p >= 0) dm_motion_clear(p);
    pthread_mutex_unlock(&g_pmu);
}
