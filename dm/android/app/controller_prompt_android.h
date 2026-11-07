// Main's controller applet waits on Android's UI thread while guest execution
// waits here. Device notifications keep player counts current without polling.
static jint controller_players(JNIEnv *env,jclass cls) { (void)env;(void)cls;return dm_players_reported(); }
int front_ctrl_support(int min,int max) {
    pthread_mutex_lock(&g_wmu);
    ANativeActivity *act=g_keyboard_live && g_win ? g_act : NULL;
    if (!act) { pthread_mutex_unlock(&g_wmu);return -1; }
    JavaVM *vm=act->vm;JNIEnv *env=NULL;int attached=0,result=-1;
    if ((*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)==JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK) { pthread_mutex_unlock(&g_wmu);return -1; }
        attached=1;
    }
    jobject activity=env ? (*env)->NewLocalRef(env,act->clazz) : NULL;
    pthread_mutex_unlock(&g_wmu);
    if (activity) {
        jclass cls=(*env)->GetObjectClass(env,activity);
        jmethodID method=cls ? (*env)->GetMethodID(env,cls,"showControllerSupport","(II)I") : NULL;
        if (method && !(*env)->ExceptionCheck(env)) {
            pthread_mutex_lock(&g_pmu);cancel_pads();pthread_mutex_unlock(&g_pmu);
            atomic_fetch_add(&g_keyboard_requests,1);
            result=(*env)->CallIntMethod(env,activity,method,min,max);
            atomic_fetch_sub(&g_keyboard_requests,1);
        }
        if (cls) (*env)->DeleteLocalRef(env,cls);
        (*env)->DeleteLocalRef(env,activity);
    }
    if (env && (*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);result=-1; }
    if (attached) (*vm)->DetachCurrentThread(vm);
    return result;
}
