/* host_miniaudio.c -- stands in for an application that already compiles
 * miniaudio, so that CI builds and links the AUDIOFILE_USE_HOST_MINIAUDIO
 * path. It compiles what src/miniaudio_impl.c does, Vorbis included, so that
 * both configurations support the same formats.
 *
 * MA_NO_DECODING is deliberately not defined: a host that defines it fails to
 * link with an undefined ma_decoder_*. The macros below are the set the core
 * is told about through AUDIOFILE_HOST_MINIAUDIO_DEFINES; the two must
 * agree.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#if AUDIOFILE_WITH_VORBIS
    #define STB_VORBIS_HEADER_ONLY
    #include "stb_vorbis.c"
#endif

#define MA_NO_DEVICE_IO
#define MA_NO_GENERATION
#define MA_NO_ENCODING

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#if AUDIOFILE_WITH_VORBIS
    #if defined(_MSC_VER) && !defined(__clang__)
        #pragma warning(push)
        #pragma warning(disable:4100)
        #pragma warning(disable:4244)
        #pragma warning(disable:4245)
        #pragma warning(disable:4456)
        #pragma warning(disable:4457)
        #pragma warning(disable:4701)
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
