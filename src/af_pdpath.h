/* af_pdpath.h -- resolves the symbol in a `read` or `open` message to a path.
 *
 * Part of pd-audiofile
 *
 * SPDX-FileCopyrightText: 2026 Jamie Bullock
 * SPDX-License-Identifier: Zlib
 */

#ifndef AF_PDPATH_H
#define AF_PDPATH_H

#include "m_pd.h"
#include <string.h>
#include <stdio.h>

/* Writes an openable path into `out`, falling back on the name as given so
 * that a missing file is reported by the decoder. */
static void af_resolve_path(t_canvas *canvas, const char *name,
                            char *out, size_t outsize)
{
    char  dir[MAXPDSTRING];
    char *filename = NULL;
    int   fd;

    dir[0] = '\0';
    fd = canvas_open(canvas, name, "", dir, &filename, MAXPDSTRING, 1);
    if (fd >= 0) {
        sys_close(fd);
        if (filename != NULL && dir[0] != '\0') {
            snprintf(out, outsize, "%s/%s", dir, filename);
            out[outsize - 1] = '\0';
            return;
        }
    }

    strncpy(out, name, outsize - 1);
    out[outsize - 1] = '\0';
}

#endif /* AF_PDPATH_H */
