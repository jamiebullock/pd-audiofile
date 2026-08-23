/* af_play_tilde.c -- the [af.play~] class: inlets, outlets and the DSP chain.
 * Decoding, resampling and buffering are in audiofile_core.
 *
 * Part of pd-audiofile
 *
 * SPDX-FileCopyrightText: 2026 Jamie Bullock
 * SPDX-License-Identifier: Zlib
 */

#include "m_pd.h"
#include "af_pdpath.h"
#include "audiofile_core.h"

#include <string.h>

#define AF_PLAY_MAX_CHANNELS 64

static t_class *af_play_class;

typedef struct _af_play {
    t_object    x_obj;
    t_canvas   *x_canvas;
    af_stream  *x_stream;
    t_clock    *x_clock;
    t_outlet   *x_msgout;

    int         x_nch;
    t_sample  **x_outvec;       /* the signal outlets, as the DSP chain sees them */
    float      *x_interleaved;  /* what the core hands back, before it is spread */
    int         x_bufframes;
} t_af_play;

static void af_play_fail(t_af_play *x, const char *reason)
{
    t_atom a;
    SETSYMBOL(&a, gensym(reason));
    outlet_anything(x->x_msgout, gensym("error"), 1, &a);
}

static void af_play_report_info(t_af_play *x, const af_info *info)
{
    t_atom out[4];
    SETFLOAT (&out[0], (t_float)info->samplerate);
    SETFLOAT (&out[1], (t_float)info->frames);
    SETFLOAT (&out[2], (t_float)info->channels);
    SETSYMBOL(&out[3], gensym(info->format));
    outlet_anything(x->x_msgout, gensym("info"), 4, out);
}

/* Pd's main thread. outlet_* is not safe from the perform routine. */
static void af_play_tick(t_af_play *x)
{
    unsigned events = af_stream_take_events(x->x_stream);

    if (events & AF_EVENT_UNDERFLOW) {
        af_play_fail(x, "underflow");
    }
    if (events & AF_EVENT_EOF) {
        af_stream_set_running(x->x_stream, 0);
        outlet_anything(x->x_msgout, gensym("eof"), 0, NULL);
    }
}

static void af_play_open(t_af_play *x, t_symbol *name)
{
    char      path[MAXPDSTRING];
    af_info   info;
    af_status status;

    if (name == NULL || name->s_name[0] == '\0') {
        af_play_fail(x, af_status_string(AF_ERR_ARGS));
        return;
    }

    af_resolve_path(x->x_canvas, name->s_name, path, sizeof(path));

    status = af_stream_open(x->x_stream, path, &info);
    if (status != AF_OK) {
        af_play_fail(x, af_status_string(status));
        return;
    }

    af_play_report_info(x, &info);
}

static void af_play_info(t_af_play *x)
{
    af_info   info;
    af_status status = af_stream_info(x->x_stream, &info);

    if (status != AF_OK) {
        af_play_fail(x, af_status_string(status));
        return;
    }
    af_play_report_info(x, &info);
}

static void af_play_close(t_af_play *x)
{
    af_stream_close(x->x_stream);
}

static void af_play_play(t_af_play *x, t_floatarg f)
{
    af_stream_set_running(x->x_stream, f != 0);
}

static void af_play_loop(t_af_play *x, t_floatarg f)
{
    af_stream_set_loop(x->x_stream, f != 0);
}

static void af_play_speed(t_af_play *x, t_floatarg f)
{
    if (!(f > 0)) {
        af_play_fail(x, "speed");
        return;
    }
    af_stream_set_speed(x->x_stream, (double)f);
}

static void af_play_pos(t_af_play *x, t_floatarg f)
{
    af_status status = af_stream_seek_seconds(x->x_stream, (double)f);
    if (status != AF_OK) af_play_fail(x, af_status_string(status));
}

static void af_play_getpos(t_af_play *x)
{
    t_atom out[2];
    SETFLOAT(&out[0], (t_float)af_stream_tell_seconds(x->x_stream));
    SETFLOAT(&out[1], (t_float)af_stream_tell_frames(x->x_stream));
    outlet_anything(x->x_msgout, gensym("pos"), 2, out);
}

static t_int *af_play_perform(t_int *w)
{
    t_af_play *x = (t_af_play *)(w[1]);
    int        n = (int)(w[2]);
    int        nch = x->x_nch;
    float     *src = x->x_interleaved;
    size_t     got;
    int        ch, i;

    got = af_stream_read(x->x_stream, src, (size_t)n);

    for (ch = 0; ch < nch; ch++) {
        t_sample *out = x->x_outvec[ch];
        for (i = 0; i < (int)got; i++) {
            out[i] = (t_sample)src[i * nch + ch];
        }
        for (; i < n; i++) {
            out[i] = 0;
        }
    }

    if (af_stream_pending(x->x_stream)) {
        clock_delay(x->x_clock, 0);
    }

    return w + 3;
}

static void af_play_dsp(t_af_play *x, t_signal **sp)
{
    int n   = sp[0]->s_n;
    int nch = x->x_nch;
    int i;

    /* No signal inlets, so sp[0] onwards are the outlets. */
    for (i = 0; i < nch; i++) {
        x->x_outvec[i] = sp[i]->s_vec;
    }

    if (n > x->x_bufframes) {
        float *grown = (float *)resizebytes(x->x_interleaved,
            (size_t)x->x_bufframes * nch * sizeof(float),
            (size_t)n * nch * sizeof(float));

        /* resizebytes has already posted the error. Keeping the buffer it
         * could not grow leaves the perform routine reading inside it. */
        if (grown == NULL) return;

        x->x_interleaved = grown;
        x->x_bufframes   = n;
    }

    af_stream_set_output_samplerate(x->x_stream, (double)sp[0]->s_sr);

    dsp_add(af_play_perform, 2, x, (t_int)n);
}

static void *af_play_new(t_floatarg fnch)
{
    t_af_play *x;
    int        nch = (int)fnch;
    int        i;
    double     sr;
    af_status  status;

    if (nch <= 0) nch = 2;
    if (nch > AF_PLAY_MAX_CHANNELS) nch = AF_PLAY_MAX_CHANNELS;

    x = (t_af_play *)pd_new(af_play_class);
    x->x_canvas = canvas_getcurrent();
    x->x_nch    = nch;

    sr = (double)sys_getsr();
    if (!(sr > 0.0)) sr = 44100.0;

    status = af_stream_new(&x->x_stream, (uint32_t)nch, sr);
    if (status != AF_OK) {
        pd_error(x, "af.play~: %s", af_status_string(status));
        pd_free((t_pd *)x);
        return NULL;
    }

    x->x_bufframes   = sys_getblksize() > 64 ? sys_getblksize() : 64;
    x->x_interleaved = (float *)getbytes((size_t)x->x_bufframes * nch * sizeof(float));
    x->x_outvec      = (t_sample **)getbytes((size_t)nch * sizeof(t_sample *));

    if (x->x_interleaved == NULL || x->x_outvec == NULL) {
        /* pd_free calls the class's free method, which releases the stream
         * and whichever of these two did come back. */
        pd_error(x, "af.play~: %s", af_status_string(AF_ERR_MEMORY));
        pd_free((t_pd *)x);
        return NULL;
    }

    for (i = 0; i < nch; i++) {
        outlet_new(&x->x_obj, &s_signal);
    }
    x->x_msgout = outlet_new(&x->x_obj, &s_anything);
    x->x_clock  = clock_new(x, (t_method)af_play_tick);

    return x;
}

static void af_play_free(t_af_play *x)
{
    /* The clock can fire while the stream exists, so it goes first. */
    if (x->x_clock) clock_free(x->x_clock);
    af_stream_free(x->x_stream);
    if (x->x_interleaved) {
        freebytes(x->x_interleaved,
                  (size_t)x->x_bufframes * x->x_nch * sizeof(float));
    }
    if (x->x_outvec) {
        freebytes(x->x_outvec, (size_t)x->x_nch * sizeof(t_sample *));
    }
}

void af_play_tilde_setup(void)
{
    af_play_class = class_new(gensym("af.play~"),
                              (t_newmethod)af_play_new,
                              (t_method)af_play_free,
                              sizeof(t_af_play), 0,
                              A_DEFFLOAT, 0);

    class_addmethod(af_play_class, (t_method)af_play_dsp,    gensym("dsp"),    A_CANT,   0);
    class_addmethod(af_play_class, (t_method)af_play_open,   gensym("open"),   A_SYMBOL, 0);
    class_addmethod(af_play_class, (t_method)af_play_info,   gensym("info"),   A_NULL);
    class_addmethod(af_play_class, (t_method)af_play_close,  gensym("close"),  A_NULL);
    class_addmethod(af_play_class, (t_method)af_play_play,   gensym("play"),   A_FLOAT,  0);
    class_addmethod(af_play_class, (t_method)af_play_loop,   gensym("loop"),   A_FLOAT,  0);
    class_addmethod(af_play_class, (t_method)af_play_speed,  gensym("speed"),  A_FLOAT,  0);
    class_addmethod(af_play_class, (t_method)af_play_pos,    gensym("pos"),    A_FLOAT,  0);
    class_addmethod(af_play_class, (t_method)af_play_getpos, gensym("getpos"), A_NULL);
}
