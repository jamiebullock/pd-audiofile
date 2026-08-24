/* af_info.c -- the [af.info] class.
 *
 * Part of pd-audiofile
 *
 * SPDX-FileCopyrightText: 2026 Jamie Bullock
 * SPDX-License-Identifier: Zlib
 */

#include "m_pd.h"
#include "af_pdpath.h"
#include "audiofile_core.h"

static t_class *af_info_class;

typedef struct _af_info {
    t_object  x_obj;
    t_canvas *x_canvas;
    t_outlet *x_out;
} t_af_info;

static void af_info_fail(t_af_info *x, af_status status)
{
    t_atom a;
    SETSYMBOL(&a, gensym(af_status_string(status)));
    outlet_anything(x->x_out, gensym("error"), 1, &a);
}

static void af_info_open(t_af_info *x, t_symbol *name)
{
    char      path[MAXPDSTRING];
    af_info   info;
    af_status status;
    t_atom    out[4];

    if (name == NULL || name->s_name[0] == '\0') {
        af_info_fail(x, AF_ERR_ARGS);
        return;
    }

    af_resolve_path(x->x_canvas, name->s_name, path, sizeof(path));

    status = af_probe(path, &info);
    if (status != AF_OK) {
        af_info_fail(x, status);
        return;
    }

    SETFLOAT (&out[0], (t_float)info.samplerate);
    SETFLOAT (&out[1], (t_float)info.duration);
    SETFLOAT (&out[2], (t_float)info.channels);
    SETSYMBOL(&out[3], gensym(info.format));

    outlet_anything(x->x_out, gensym("info"), 4, out);
}

static void *af_info_new(void)
{
    t_af_info *x = (t_af_info *)pd_new(af_info_class);
    x->x_canvas = canvas_getcurrent();
    x->x_out    = outlet_new(&x->x_obj, &s_anything);
    return x;
}

void af_info_setup(void)
{
    af_info_class = class_new(gensym("af.info"),
                              (t_newmethod)af_info_new, 0,
                              sizeof(t_af_info), 0, 0);

    class_addmethod(af_info_class, (t_method)af_info_open,
                    gensym("open"), A_SYMBOL, 0);
}
