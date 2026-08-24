/* audiofile_setup.c -- the library entry point, called by Pd on -lib audiofile.
 *
 * Part of pd-audiofile
 *
 * SPDX-FileCopyrightText: 2026 Jamie Bullock
 * SPDX-License-Identifier: Zlib
 */

#include "m_pd.h"

#ifndef AUDIOFILE_VERSION
#error "define AUDIOFILE_VERSION: CMake passes the project version, other builds must too"
#endif

#if defined(_WIN32)
#define AF_LIBRARY_ENTRY __declspec(dllexport)
#else
#define AF_LIBRARY_ENTRY
#endif

void af_info_setup(void);
void af_play_tilde_setup(void);

AF_LIBRARY_ENTRY void audiofile_setup(void)
{
    af_info_setup();
    af_play_tilde_setup();

    post("pd-audiofile %s: [af.info] [af.play~]", AUDIOFILE_VERSION);
}
