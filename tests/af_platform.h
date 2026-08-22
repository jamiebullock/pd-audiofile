/* af_platform.h -- a millisecond sleep and a thread, for the tests. Nothing in
 * the library uses either: the only threads it deals with are miniaudio's and
 * Pd's.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#ifndef AF_PLATFORM_H
#define AF_PLATFORM_H

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <pthread.h>
    #include <time.h>
#endif

#if defined(_MSC_VER)
    #define AF_INLINE static __inline
#else
    #define AF_INLINE static inline
#endif

typedef struct {
    void (*entry)(void *);
    void  *arg;
#if defined(_WIN32)
    HANDLE handle;
#else
    pthread_t thread;
#endif
} af_thread;

#if defined(_WIN32)

AF_INLINE void af_sleep_ms(int ms) { Sleep((DWORD)ms); }

AF_INLINE DWORD WINAPI af_thread_trampoline(LPVOID data)
{
    af_thread *t = (af_thread *)data;
    t->entry(t->arg);
    return 0;
}

AF_INLINE int af_thread_start(af_thread *t, void (*entry)(void *), void *arg)
{
    t->entry  = entry;
    t->arg    = arg;
    t->handle = CreateThread(NULL, 0, af_thread_trampoline, t, 0, NULL);
    return t->handle != NULL;
}

AF_INLINE void af_thread_join(af_thread *t)
{
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
}

#else

AF_INLINE void af_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

AF_INLINE void *af_thread_trampoline(void *data)
{
    af_thread *t = (af_thread *)data;
    t->entry(t->arg);
    return NULL;
}

AF_INLINE int af_thread_start(af_thread *t, void (*entry)(void *), void *arg)
{
    t->entry = entry;
    t->arg   = arg;
    return pthread_create(&t->thread, NULL, af_thread_trampoline, t) == 0;
}

AF_INLINE void af_thread_join(af_thread *t)
{
    pthread_join(t->thread, NULL);
}

#endif

#endif /* AF_PLATFORM_H */
