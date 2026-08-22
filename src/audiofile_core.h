/* audiofile_core.h -- opening sound files and playing them. Includes no Pd
 * header, so the tests can have it without one.
 *
 * Two threads share a stream. The control thread sends messages, takes the
 * events, and may block briefly closing a file; the audio thread calls
 * af_stream_read and af_stream_pending, and nothing else, and never blocks.
 * Reading ahead from disk is miniaudio's own thread, which neither waits
 * for.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
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
    uint64_t    frames;     /* 0 when the container does not record a length */
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

/* --- control thread --- */

/* Opens `path` and reports its header in `info`. Leaves the stream stopped at
 * frame 0, replacing any file already open. A failed open changes nothing. */
af_status af_stream_open(af_stream *s, const char *path, af_info *info);

/* The header of the file currently open. AF_ERR_NOFILE if there is none. */
af_status af_stream_info(af_stream *s, af_info *info);

void af_stream_close(af_stream *s);

void af_stream_set_running(af_stream *s, int running);
void af_stream_set_loop(af_stream *s, int loop);

/* 1 is the file's own rate. Values <= 0 are ignored, the rest clamped. */
void af_stream_set_speed(af_stream *s, double speed);

/* Pd's sample rate. */
void af_stream_set_output_samplerate(af_stream *s, double samplerate);

/* In seconds, or in frames of the file's own rate. Past the end clamps. */
af_status af_stream_seek_seconds(af_stream *s, double seconds);
af_status af_stream_seek_frames(af_stream *s, uint64_t frame);

/* Where the play head is. Safe to call while the audio thread is reading. */
uint64_t af_stream_tell_frames(af_stream *s);
double   af_stream_tell_seconds(af_stream *s);

/* --- audio thread --- */

/* Writes up to `frames` interleaved frames into `dst` and returns how many;
 * the caller silences the remainder. Allocates nothing and never blocks. */
size_t af_stream_read(af_stream *s, float *dst, size_t frames);

/* Nonzero when there is an event waiting, without consuming it. */
int af_stream_pending(af_stream *s);

/* Takes every event raised since the last call; each is raised once. Safe
 * from either thread, and meant for the control one. */
typedef enum {
    AF_EVENT_EOF       = 1 << 0,
    AF_EVENT_UNDERFLOW = 1 << 1
} af_event;

unsigned af_stream_take_events(af_stream *s);

#define AF_SPEED_MIN 0.001
#define AF_SPEED_MAX 32.0

#ifdef __cplusplus
}
#endif

#endif /* AUDIOFILE_CORE_H */
