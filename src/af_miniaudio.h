/* af_miniaudio.h -- the single place miniaudio.h is included, so that every
 * translation unit sees it configured identically.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#ifndef AF_MINIAUDIO_H
#define AF_MINIAUDIO_H

#if !AUDIOFILE_USE_HOST_MINIAUDIO || AUDIOFILE_STUB_COMPILES_MINIAUDIO
    /* Pd owns the audio device. */
    #ifndef MA_NO_DEVICE_IO
    #define MA_NO_DEVICE_IO
    #endif
    #ifndef MA_NO_GENERATION
    #define MA_NO_GENERATION
    #endif
    #ifndef MA_NO_ENCODING
    #define MA_NO_ENCODING
    #endif
#endif

#include "miniaudio.h"

#endif /* AF_MINIAUDIO_H */
