/* audiofile_core.h -- opening sound files and playing them. Includes no Pd
 * header, so the tests can have it without one.
 *
 * One thread owns a stream: Pd runs messages and the perform routine on its
 * scheduler thread, and a libpd host has to serialise its calls the same way
 * libpd itself requires. Reading ahead from disk happens on miniaudio's own
 * thread, which this code never waits for.
 *
 * Part of pd-audiofile
 *
 * SPDX-FileCopyrightText: 2026 Jamie Bullock
 * SPDX-License-Identifier: Zlib
 */

#ifndef AUDIOFILE_CORE_H
#define AUDIOFILE_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AF_OK = 0,
    AF_ERR_ARGS,       /* a bad argument reached us */
    AF_ERR_OPEN,       /* the file could not be opened */
    AF_ERR_FORMAT,     /* opened, but not something we can decode */
    AF_ERR_MEMORY,
    AF_ERR_NOFILE,     /* the operation needs a file and none is open */
    AF_ERR_SEEK,
    AF_ERR_CONFIG      /* built against a differently configured miniaudio */
} af_status;

/* A short, stable word naming the failure. */
const char *af_status_string(af_status status);

typedef struct {
    double      samplerate;
    double      duration;   /* seconds; 0 when the container records no length */
    uint32_t    channels;
    const char *format;     /* "u8", "s16", "s24", "s32", "f32" or "unknown" */
} af_info;

/* Reads a file's header. Reads no audio. */
af_status af_probe(const char *path, af_info *out);

typedef struct af_stream af_stream;

/* `channels` is fixed for the life of the stream, and files are mixed to it. */
af_status af_stream_new(af_stream **out, uint32_t channels,
                        double out_samplerate);
void      af_stream_free(af_stream *s);

/* Opens `path` and reports its header in `info`. Leaves the stream stopped at
 * frame 0, replacing any file already open. A failed open changes nothing.
 * Opens the file twice: the second stream is the one a cue prepares. */
af_status af_stream_open(af_stream *s, const char *path, af_info *info);

/* The header of the file currently open. AF_ERR_NOFILE if there is none. */
af_status af_stream_info(af_stream *s, af_info *info);

void af_stream_close(af_stream *s);

void af_stream_set_playing(af_stream *s, int playing);
void af_stream_set_looping(af_stream *s, int looping);
void af_stream_set_autorestart(af_stream *s, int autorestart);

/* 1 is the file's own rate. Values <= 0 are ignored, the rest clamped. A cue
 * not yet taken keeps the speed it was made at; the place it moves to plays at
 * this one. */
void af_stream_set_speed(af_stream *s, double speed);

/* Pd's sample rate. */
void af_stream_set_output_samplerate(af_stream *s, double samplerate);

/* In seconds. Past the end clamps. */
af_status af_stream_seek_seconds(af_stream *s, double seconds);

/* Where the play head is. */
double af_stream_tell_seconds(af_stream *s);

/* Prepares a seek to `seconds` on a second voice while the current place keeps
 * playing, with the `preroll` seconds of the file before it to be handed over.
 * AF_EVENT_CUED is raised once the new place is buffered, and
 * af_stream_take_cue then makes the switch. Past the end clamps, as a seek
 * does. A cue replaces one not yet taken, and a seek, open or close cancels
 * it. The place being left plays on until then; if it reaches its end first it
 * gives silence and no AF_EVENT_EOF. Blocks on nothing. The pre-roll is at
 * most AF_CUE_PREROLL_MAX seconds of the file, and as long again of output at
 * the current speed. */
af_status af_stream_cue(af_stream *s, double seconds, double preroll);

/* The pre-roll af_stream_take_cue will write, in frames at Pd's rate and the
 * speed set when the cue was made. 0 when no cue is outstanding. */
size_t af_stream_cue_frames(af_stream *s);

/* After AF_EVENT_CUED: writes the pre-roll, interleaved, into `dst`, which
 * holds af_stream_cue_frames frames or is NULL, and switches to the cued
 * place, so that the next read continues from the cue point. `t0` is the file
 * time of the pre-roll's first frame, negative where the pre-roll starts
 * before the file and is filled with silence; `seconds` is the cue point.
 * AF_ERR_ARGS if no cue is ready, as after one was replaced or cancelled.
 * AF_ERR_SEEK if the pre-roll came back short, when the rest of it is
 * silence; the switch is made all the same. */
af_status af_stream_take_cue(af_stream *s, float *dst, double *t0,
                             double *seconds);

/* Writes up to `frames` interleaved frames into `dst` and returns how many;
 * the caller silences the remainder. Meant for a perform routine: it
 * allocates nothing, takes no lock and never blocks. */
size_t af_stream_read(af_stream *s, float *dst, size_t frames);

/* Nonzero when there is an event waiting, without consuming it. */
int af_stream_pending(af_stream *s);

/* Reads every event raised since the last call; each is raised once. */
typedef enum {
    AF_EVENT_EOF       = 1 << 0,
    AF_EVENT_UNDERFLOW = 1 << 1,
    AF_EVENT_CUED      = 1 << 2
} af_event;

unsigned af_stream_read_events(af_stream *s);

#define AF_SPEED_MIN 0.001
#define AF_SPEED_MAX 32.0

/* Seconds: what a seek buffers holds the pre-roll and the blocks after it. */
#define AF_CUE_PREROLL_MAX 0.5

#ifdef __cplusplus
}
#endif

#endif /* AUDIOFILE_CORE_H */
