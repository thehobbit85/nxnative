// Included by front_android.c after its shared controller state and mapping.
// Commands enqueue UI work; callbacks carry a generation so an old view cannot
// deliver an event into a replacement guest session.
static _Atomic uint64_t g_web_generation;
static int web_command(int command, const char *a, size_t an, const char *b, const char *c, const char *d,
                       int hidden, int delay, int pulse, int muted) {
    if (!g_act) return -1;
    JNIEnv *env = NULL; int attached = 0;
    if ((*g_act->vm)->GetEnv(g_act->vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*g_act->vm)->AttachCurrentThread(g_act->vm, &env, NULL) != JNI_OK) return -1;
        attached = 1;
    }
    if (!env) return -1;
    jclass cls = (*env)->GetObjectClass(env, g_act->clazz);
    jmethodID method = (*env)->GetMethodID(env, cls, "webCommand", "(JI[B[B[B[BIIII)V");
    jbyteArray args[4] = {0}; const char *text[4] = {a, b, c, d};
    for (int i = 0; i < 4; i++) if (text[i]) {
        size_t n = i ? strlen(text[i]) : an;
        args[i] = (*env)->NewByteArray(env, (jsize)n);
        if (args[i]) (*env)->SetByteArrayRegion(env, args[i], 0, (jsize)n, (const jbyte *)text[i]);
    }
    if (method && !(*env)->ExceptionCheck(env)) (*env)->CallVoidMethod(env, g_act->clazz, method,
        (jlong)atomic_load(&g_web_generation), command, args[0], args[1], args[2], args[3], hidden, delay, pulse, muted);
    int failed = !method || (*env)->ExceptionCheck(env);
    if (failed) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); LOG("[RED] web: Android UI command failed"); }
    for (int i = 0; i < 4; i++) if (args[i]) (*env)->DeleteLocalRef(env, args[i]);
    (*env)->DeleteLocalRef(env, cls);
    if (attached) (*g_act->vm)->DetachCurrentThread(g_act->vm);
    return failed ? -1 : 0;
}
int front_web_open(const char *dir, const char *path, const char *callback, int hidden, const dm_web_opts *opts) {
    atomic_store(&g_web_generation, opts ? opts->session : 0);
    char font[1024]; const char *ext = getenv("DM_SHARED_FONT_EXT"), *fonts = getenv("DM_FONT_DIR"), *audio = getenv("DM_AUDIO");
    snprintf(font, sizeof font, "%s%s", ext ? ext : fonts ? fonts : "", ext || !fonts ? "" : "/nintendo_ext.ttf");
    return web_command(0, dir, strlen(dir), path, callback, font, hidden, opts ? opts->repeat_delay : 0, opts ? opts->repeat_pulse : 0, audio && !atoi(audio));
}
void front_web_show(void) { web_command(1, NULL, 0, NULL, NULL, NULL, 0, 0, 0, 0); }
void front_web_send(const char *message, size_t n) { web_command(2, message, n, NULL, NULL, NULL, 0, 0, 0, 0); }
void front_web_close(void) { web_command(3, NULL, 0, NULL, NULL, NULL, 0, 0, 0, 0); }
JNIEXPORT void JNICALL Java_com_nxnative_GameActivity_nativeWebEvent(JNIEnv *env, jclass cls, jlong id, jint kind, jint reason, jbyteArray message) {
    (void)cls;
    size_t n = message ? (size_t)(*env)->GetArrayLength(env, message) : 0;
    char *text = malloc(n + 1); if (!text) return;
    if (n) (*env)->GetByteArrayRegion(env, message, 0, (jsize)n, (jbyte *)text); text[n] = 0;
    if (kind == 0) dm_web_message_for((uint64_t)id, text, n);
    else if (kind == 1) dm_web_ended_for((uint64_t)id, (uint32_t)reason, text);
    else if (kind == 2) dm_web_failed_for((uint64_t)id, text);
    else if (kind == 3) { static unsigned logged; if (logged++ < 200) LOG("web: page %s", text); }
    free(text);
}
JNIEXPORT jlongArray JNICALL Java_com_nxnative_GameActivity_nativeWebPad(JNIEnv *env, jclass cls) {
    (void)cls; uint64_t buttons; int32_t axes[4]; dm_input_peek(&buttons, axes);
    jlong data[5] = {(jlong)buttons, axes[0], axes[1], axes[2], axes[3]};
    jlongArray out = (*env)->NewLongArray(env, 5); if (out) (*env)->SetLongArrayRegion(env, out, 0, 5, data); return out;
}
JNIEXPORT jboolean JNICALL Java_com_nxnative_GameActivity_nativeWebKey(JNIEnv *env, jclass cls, jint device, jint key, jint action) {
    (void)env; (void)cls; uint64_t bit = key_bit(key); if (!bit) return JNI_FALSE;
    pthread_mutex_lock(&g_pmu); int s = dev_slot(device);
    if (action == AKEY_EVENT_ACTION_DOWN) g_dev[s].btn |= bit;
    else if (action == AKEY_EVENT_ACTION_UP) g_dev[s].btn &= ~bit;
    pads_push_(); pthread_mutex_unlock(&g_pmu); return JNI_TRUE;
}
JNIEXPORT void JNICALL Java_com_nxnative_GameActivity_nativeWebAxes(JNIEnv *env, jclass cls, jint device, jfloatArray values) {
    (void)cls; if (!values || (*env)->GetArrayLength(env, values) != 10) return;
    float axes[10]; (*env)->GetFloatArrayRegion(env, values, 0, 10, axes);
    pthread_mutex_lock(&g_pmu); int s = dev_slot(device);
    memcpy(g_dev[s].ax, axes, sizeof axes); pads_push_(); pthread_mutex_unlock(&g_pmu);
}
static void web_register(JNIEnv *env, jobject activity) {
    // NativeActivity dlopens the host itself; it does not associate that dlopen
    // with GameActivity's Java class loader as System.loadLibrary would.
    const JNINativeMethod methods[] = {
        {"nativeWebEvent", "(JII[B)V", (void *)Java_com_nxnative_GameActivity_nativeWebEvent},
        {"nativeWebPad", "()[J", (void *)Java_com_nxnative_GameActivity_nativeWebPad},
        {"nativeWebKey", "(III)Z", (void *)Java_com_nxnative_GameActivity_nativeWebKey},
        {"nativeWebAxes", "(I[F)V", (void *)Java_com_nxnative_GameActivity_nativeWebAxes},
    };
    jclass cls = (*env)->GetObjectClass(env, activity);
    if ((*env)->RegisterNatives(env, cls, methods, sizeof methods / sizeof *methods)) die("web: cannot register Android callbacks");
    (*env)->DeleteLocalRef(env, cls);
}
