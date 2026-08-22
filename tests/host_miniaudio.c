/* host_miniaudio.c -- stands in for an application that already compiles
 * miniaudio, so that CI builds and links the AUDIOFILE_USE_HOST_MINIAUDIO
 * path. Compiling src/miniaudio_impl.c is what a host does; the macros it
 * sets are the ones CMake passes back as AUDIOFILE_HOST_MINIAUDIO_DEFINES.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#define AUDIOFILE_STUB_COMPILES_MINIAUDIO 1
#include "miniaudio_impl.c"
