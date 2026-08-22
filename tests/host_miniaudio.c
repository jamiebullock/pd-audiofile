/* host_miniaudio.c -- stands in for an application that already compiles
 * miniaudio, so that CI builds and links the
 * AUDIOFILE_MINIAUDIO_IMPLEMENTATION=OFF path.
 *
 * MA_NO_DECODING is deliberately not defined: a host that defines it fails to
 * link with an undefined ma_decoder_*. The macros below are the set the core
 * is told about through AUDIOFILE_MINIAUDIO_DEFINES; the two must agree.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#define MA_NO_DEVICE_IO
#define MA_NO_GENERATION
#define MA_NO_ENCODING

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
