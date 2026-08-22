/* af_platform.h -- a millisecond sleep, for the tests.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#ifndef AF_PLATFORM_H
#define AF_PLATFORM_H

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <time.h>
#endif

#if defined(_MSC_VER)
    #define AF_INLINE static __inline
#else
    #define AF_INLINE static inline
#endif

#if defined(_WIN32)

AF_INLINE void af_sleep_ms(int ms) { Sleep((DWORD)ms); }

#else

AF_INLINE void af_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

#endif

#endif /* AF_PLATFORM_H */
