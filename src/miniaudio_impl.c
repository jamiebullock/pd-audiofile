/* miniaudio_impl.c -- the one translation unit that compiles miniaudio.
 * Excluded from the build when AUDIOFILE_USE_HOST_MINIAUDIO is on.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#if !AUDIOFILE_USE_HOST_MINIAUDIO || AUDIOFILE_STUB_COMPILES_MINIAUDIO

/* stb_vorbis calls fopen(). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

/* miniaudio compiles its Vorbis backend only when stb_vorbis' header guard is
 * already visible, so the declarations precede it and the implementation
 * follows it. */
#if AUDIOFILE_WITH_VORBIS
    #define STB_VORBIS_HEADER_ONLY
    #include "stb_vorbis.c"
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "af_miniaudio.h"

#if AUDIOFILE_WITH_VORBIS
    #if defined(_MSC_VER) && !defined(__clang__)
        #pragma warning(push)
        #pragma warning(disable:4100)   /* unreferenced formal parameter */
        #pragma warning(disable:4244)   /* possible loss of data */
        #pragma warning(disable:4245)   /* signed/unsigned mismatch */
        #pragma warning(disable:4456)   /* declaration hides previous local */
        #pragma warning(disable:4457)   /* declaration hides function parameter */
        #pragma warning(disable:4701)   /* potentially uninitialized local */
    #elif defined(__GNUC__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wunused-value"
    #endif
    #undef STB_VORBIS_HEADER_ONLY
    #include "stb_vorbis.c"
    #if defined(_MSC_VER) && !defined(__clang__)
        #pragma warning(pop)
    #elif defined(__GNUC__)
        #pragma GCC diagnostic pop
    #endif
#endif

#else

/* ISO C forbids an empty translation unit. */
typedef int af_host_compiles_miniaudio;

#endif
