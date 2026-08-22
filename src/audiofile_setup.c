/* audiofile_setup.c -- the library entry point, called by Pd on -lib audiofile.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#include "m_pd.h"

#ifndef AUDIOFILE_VERSION
#define AUDIOFILE_VERSION "0.1.0"
#endif

void af_info_setup(void);
void af_play_tilde_setup(void);

void audiofile_setup(void)
{
    af_info_setup();
    af_play_tilde_setup();

    post("pd-audiofile %s: [af.info] [af.play~]", AUDIOFILE_VERSION);
}
