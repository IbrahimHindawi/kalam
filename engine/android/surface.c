#include "surface.h"
#include <android/native_window_jni.h>
#include <pthread.h>

/* Android's UI thread publishes SurfaceHolder lifetimes. bgfx uses a separate
   reference so a callback can retire the Java Surface during a frame safely. */
static pthread_mutex_t surface_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *published;
static uint64_t published_generation;

JNIEXPORT void JNICALL
Java_org_kalam_renderer_KalamActivity_publishSurface(JNIEnv *env, jclass klass, jobject surface)
{
    (void)klass;
    ANativeWindow *next = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&surface_lock);
    ANativeWindow *old = published;
    published = next;
    ++published_generation;
    pthread_mutex_unlock(&surface_lock);
    if (old) ANativeWindow_release(old);
}

void *kh_android_acquire_surface(uint64_t *generation)
{
    pthread_mutex_lock(&surface_lock);
    ANativeWindow *window = published;
    if (window) ANativeWindow_acquire(window);
    *generation = published_generation;
    pthread_mutex_unlock(&surface_lock);
    return window;
}

void kh_android_release_surface(void *window)
{
    if (window) ANativeWindow_release((ANativeWindow *)window);
}
