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
 * frame 0, replacing any file already open. A failed open changes nothing. */
af_status af_stream_open(af_stream *s, const char *path, af_info *info);

/* The header of the file currently open. AF_ERR_NOFILE if there is none. */
af_status af_stream_info(af_stream *s, af_info *info);

void af_stream_close(af_stream *s);

void af_stream_set_playing(af_stream *s, int playing);
void af_stream_set_looping(af_stream *s, int looping);

/* Whether playing a file that has reached its end rewinds it first. A stream
   sitting at its end has nothing left to read, so without this it stays
   silent until something seeks it. On by default. */
void af_stream_set_autorestart(af_stream *s, int autorestart);

/* 1 is the file's own rate. Values <= 0 are ignored, the rest clamped. */
void af_stream_set_speed(af_stream *s, double speed);

/* Pd's sample rate. */
void af_stream_set_output_samplerate(af_stream *s, double samplerate);

/* In seconds. Past the end clamps. */
af_status af_stream_seek_seconds(af_stream *s, double seconds);

/* Where the play head is. */
double af_stream_tell_seconds(af_stream *s);

/* Writes up to `frames` interleaved frames into `dst` and returns how many;
 * the caller silences the remainder. Meant for a perform routine: it
 * allocates nothing, takes no lock and never blocks. */
size_t af_stream_read(af_stream *s, float *dst, size_t frames);

/* Nonzero when there is an event waiting, without consuming it. */
int af_stream_pending(af_stream *s);

/* Reads every event raised since the last call; each is raised once. */
typedef enum {
    AF_EVENT_EOF       = 1 << 0,
    AF_EVENT_UNDERFLOW = 1 << 1
} af_event;

unsigned af_stream_read_events(af_stream *s);

#define AF_SPEED_MIN 0.001
#define AF_SPEED_MAX 32.0

#ifdef __cplusplus
}
#endif

#endif /* AUDIOFILE_CORE_H */
