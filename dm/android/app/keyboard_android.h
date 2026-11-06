// Shared swkbd owns the guest encodings and UTF-16 length contract. Use byte
// arrays for ordinary UTF-8 across JNI: NewStringUTF uses modified UTF-8.
extern int g_swkbd_min_chars;
static _Atomic unsigned g_keyboard_requests;
static int g_keyboard_live;   // g_wmu guards the activity while the guest obtains its JNI reference
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial,
                     int max_chars, char *out, int out_cap) {
    if (!out || out_cap < 1) return -1;
    pthread_mutex_lock(&g_wmu);
    ANativeActivity *act = g_keyboard_live && g_win ? g_act : NULL;
    if (!act) { pthread_mutex_unlock(&g_wmu); return -1; }
    JavaVM *vm = act->vm;
    JNIEnv *env = NULL; int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
            pthread_mutex_unlock(&g_wmu); return -1;
        }
        attached = 1;
    }
    int result = -1;
    jobject activity = env ? (*env)->NewLocalRef(env, act->clazz) : NULL;
    pthread_mutex_unlock(&g_wmu);
    if (activity && !(*env)->PushLocalFrame(env, 8)) {
        jclass cls = (*env)->GetObjectClass(env, activity);
        jmethodID method = cls ? (*env)->GetMethodID(env, cls, "readKeyboard", "([B[B[B[BII)[B") : NULL;
        const char *args[] = {title, sub, guide, initial};
        jbyteArray bytes[4] = {0};
        for (int i = 0; method && i < 4 && !(*env)->ExceptionCheck(env); i++) {
            jsize n = args[i] ? (jsize)strlen(args[i]) : 0;
            bytes[i] = (*env)->NewByteArray(env, n);
            if (bytes[i] && n) (*env)->SetByteArrayRegion(env, bytes[i], 0, n, (const jbyte *)args[i]);
        }
        if (method && !(*env)->ExceptionCheck(env)) {
            atomic_fetch_add(&g_keyboard_requests, 1);
            jbyteArray answer = (*env)->CallObjectMethod(env, activity, method, bytes[0], bytes[1], bytes[2], bytes[3],
                                                          g_swkbd_min_chars, max_chars);
            atomic_fetch_sub(&g_keyboard_requests, 1);
            if (!(*env)->ExceptionCheck(env)) {
                if (!answer) result = 0;
                else {
                    jsize len = (*env)->GetArrayLength(env, answer);
                    jsize n = len < out_cap ? len : out_cap - 1;
                    // Inspect the byte following the cut to avoid returning half a code point.
                    (*env)->GetByteArrayRegion(env, answer, 0, n + (n < len), (jbyte *)out);
                    if (n < len) while (n && ((unsigned char)out[n] & 0xc0) == 0x80) --n;
                    out[n] = 0; result = 1;
                }
            }
        }
        (*env)->PopLocalFrame(env, NULL);
    }
    if (env && (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); result = -1;
    }
    if (activity) (*env)->DeleteLocalRef(env, activity);
    if (attached) (*vm)->DetachCurrentThread(vm);
    return result;
}
