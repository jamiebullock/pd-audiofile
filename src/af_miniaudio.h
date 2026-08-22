/* af_miniaudio.h -- the single place miniaudio.h is included, so that every
 * translation unit sees it configured identically.
 *
 * With AUDIOFILE_MINIAUDIO_IMPLEMENTATION off, the host application has
 * already compiled miniaudio, and must have left decoding, the engine, the
 * node graph and the resource manager all enabled. Its MA_NO_* macros are part
 * of the layout of ma_engine_config, so the same set has to reach this header:
 * pass them in AUDIOFILE_MINIAUDIO_DEFINES. Getting that wrong is not a link
 * error -- af_stream_new answers AF_ERR_CONFIG.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#ifndef AF_MINIAUDIO_H
#define AF_MINIAUDIO_H

#if AUDIOFILE_MINIAUDIO_IMPLEMENTATION
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
