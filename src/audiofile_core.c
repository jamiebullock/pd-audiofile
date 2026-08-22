/* audiofile_core.c -- one ma_engine per stream, in no-device mode, and one
 * ma_sound per open file. The header has the thread contract, the README the
 * buffering.
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#include "audiofile_core.h"
#include "af_miniaudio.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Also sizes the node caches a seek plays through, so one Pd block of stale
 * audio rather than the 480-frame default. */
#define AF_PERIOD_FRAMES 64

/* One sound into the endpoint, against a default of half a megabyte per
 * channel. */
#define AF_PREMIX_STACK_BYTES 16384

#define AF_FALLBACK_SAMPLERATE 44100.0

static const af_info af_info_none = { 0.0, 0, 0, "unknown" };

struct af_stream {
    uint32_t channels;
    double   out_rate;
    double   speed;         /* control thread; the sound carries it as pitch */

    ma_engine engine;
    bool      engine_ready;

    ma_sound *sound;

    char    *path;
    af_info  info;

    /* The data source always loops, so the pages either side of the loop
     * point are filled knowing they wrap, and a file played once is stopped
     * here by counting `length` down. False when no length is known. */
    bool engine_loops;

    /* In the engine's frames; 0 when the container records no length. */
    uint64_t length;

    bool live;                  /* a file is open */
    bool running;
    bool loop;

    /* What the caller has been handed, in the engine's frames. The sound's own
     * cursor is the read head, which runs ahead of it. */
    double position;

    /* Set by a seek, cleared by the block that seek is applied in. */
    bool settling;

    bool eof_seen;
    bool starved;
    bool primed;

    unsigned events;
};

const char *af_status_string(af_status status)
{
    switch (status) {
        case AF_OK:         return "ok";
        case AF_ERR_ARGS:   return "badargs";
        case AF_ERR_OPEN:   return "nofile";
        case AF_ERR_FORMAT: return "format";
        case AF_ERR_MEMORY: return "memory";
        case AF_ERR_NOFILE: return "noopen";
        case AF_ERR_SEEK:   return "seek";
        case AF_ERR_CONFIG: return "config";
        default:            return "unknown";
    }
}

static af_status af_status_from_ma(ma_result result)
{
    switch (result) {
        case MA_SUCCESS:          return AF_OK;
        case MA_DOES_NOT_EXIST:   return AF_ERR_OPEN;
        case MA_ACCESS_DENIED:    return AF_ERR_OPEN;
        case MA_OUT_OF_MEMORY:    return AF_ERR_MEMORY;
        case MA_INVALID_ARGS:     return AF_ERR_ARGS;
        case MA_INVALID_FILE:     return AF_ERR_FORMAT;
        case MA_NO_BACKEND:       return AF_ERR_FORMAT;
        default:                  return AF_ERR_FORMAT;
    }
}

static const char *af_format_name(ma_format format)
{
    switch (format) {
        case ma_format_u8:  return "u8";
        case ma_format_s16: return "s16";
        case ma_format_s24: return "s24";
        case ma_format_s32: return "s32";
        case ma_format_f32: return "f32";
        default:            return "unknown";
    }
}

static char *af_copy_string(const char *text)
{
    size_t size = strlen(text) + 1;
    char  *copy = (char *)malloc(size);

    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

/* The decoder must have had no format or channel count forced on it, or it
 * reports the settings it was given rather than the file's. */
static af_status af_read_info(ma_decoder *decoder, af_info *out)
{
    ma_format  format      = ma_format_unknown;
    ma_uint32  channels    = 0;
    ma_uint32  samplerate  = 0;
    ma_uint64  frames      = 0;
    ma_result  result      = MA_ERROR;

    if (decoder->pBackend != NULL) {
        result = ma_data_source_get_data_format(decoder->pBackend, &format,
                                                &channels, &samplerate, NULL, 0);
    }
    if (result != MA_SUCCESS) {
        result = ma_decoder_get_data_format(decoder, &format, &channels,
                                            &samplerate, NULL, 0);
    }
    if (result != MA_SUCCESS || channels == 0 || samplerate == 0) {
        return AF_ERR_FORMAT;
    }

    if (ma_decoder_get_length_in_pcm_frames(decoder, &frames) != MA_SUCCESS) {
        frames = 0;
    }

    out->samplerate = (double)samplerate;
    out->frames     = (uint64_t)frames;
    out->channels   = (uint32_t)channels;
    out->format     = af_format_name(format);
    return AF_OK;
}

af_status af_probe(const char *path, af_info *out)
{
    ma_decoder_config config;
    ma_decoder decoder;
    ma_result  result;
    af_status  status;

    if (path == NULL || out == NULL) return AF_ERR_ARGS;

    *out = af_info_none;

    config = ma_decoder_config_init(ma_format_unknown, 0, 0);
    result = ma_decoder_init_file(path, &config, &decoder);
    if (result != MA_SUCCESS) return af_status_from_ma(result);

    status = af_read_info(&decoder, out);
    ma_decoder_uninit(&decoder);
    return status;
}

/* The sound counts in the engine's frames, the interface in the file's. */
static double af_engine_per_file(const af_stream *s)
{
    if (s->info.samplerate <= 0.0) return 1.0;
    return s->out_rate / s->info.samplerate;
}

static uint64_t af_to_engine_frames(const af_stream *s, uint64_t file_frame)
{
    return (uint64_t)((double)file_frame * af_engine_per_file(s) + 0.5);
}

static uint64_t af_to_file_frames(const af_stream *s, uint64_t engine_frame)
{
    return (uint64_t)((double)engine_frame / af_engine_per_file(s) + 0.5);
}

/* Not the sound's cursor, which already reports a seek that has yet to
 * happen. */
static uint64_t af_source_cursor(ma_sound *sound)
{
    ma_uint64 cursor = 0;
    ma_data_source *source = (sound != NULL) ? ma_sound_get_data_source(sound) : NULL;

    if (source == NULL) return 0;
    ma_data_source_get_cursor_in_pcm_frames(source, &cursor);
    return (uint64_t)cursor;
}

static uint64_t af_available(ma_sound *sound)
{
    ma_uint64 available = 0;
    ma_data_source *source = (sound != NULL) ? ma_sound_get_data_source(sound) : NULL;

    if (source == NULL) return 0;
    ma_resource_manager_data_source_get_available_frames(
        (ma_resource_manager_data_source *)source, &available);
    return (uint64_t)available;
}

static af_status af_engine_start(af_stream *s)
{
    ma_engine_config config = ma_engine_config_init();

    /* This ran in whichever unit compiled miniaudio: a count of anything but
     * one means it saw different MA_NO_* macros, and the two disagree about
     * where the fields of this struct are. */
    if (config.listenerCount != 1) return AF_ERR_CONFIG;

    config.noDevice               = MA_TRUE;
    config.channels               = s->channels;
    config.sampleRate             = (ma_uint32)(s->out_rate + 0.5);
    config.periodSizeInFrames     = AF_PERIOD_FRAMES;
    config.preMixStackSizeInBytes = AF_PREMIX_STACK_BYTES;

    if (ma_engine_init(&config, &s->engine) != MA_SUCCESS) return AF_ERR_MEMORY;
    s->engine_ready = true;
    return AF_OK;
}

/* The engine holds it in its node graph until it is uninitialised. */
static void af_free_sound(ma_sound *sound)
{
    if (sound == NULL) return;
    ma_sound_uninit(sound);
    free(sound);
}

static void af_retire_sound(af_stream *s)
{
    af_free_sound(s->sound);
    s->sound = NULL;
}

static void af_engine_stop(af_stream *s)
{
    af_retire_sound(s);
    if (s->engine_ready) {
        ma_engine_uninit(&s->engine);
        s->engine_ready = false;
    }
}

af_status af_stream_new(af_stream **out, uint32_t channels,
                        double out_samplerate)
{
    af_stream *s;
    af_status  status;

    if (out == NULL || channels == 0 || channels > MA_MAX_CHANNELS) {
        return AF_ERR_ARGS;
    }
    *out = NULL;

    if (out_samplerate <= 0.0) out_samplerate = AF_FALLBACK_SAMPLERATE;

    s = (af_stream *)calloc(1, sizeof(*s));
    if (s == NULL) return AF_ERR_MEMORY;

    s->channels = channels;
    s->out_rate = out_samplerate;
    s->info     = af_info_none;
    s->speed    = 1.0;

    status = af_engine_start(s);
    if (status != AF_OK) {
        free(s);
        return status;
    }

    *out = s;
    return AF_OK;
}

void af_stream_free(af_stream *s)
{
    if (s == NULL) return;

    af_engine_stop(s);
    free(s->path);
    free(s);
}

static void af_forget_position(af_stream *s, double position)
{
    s->position = position;
    s->eof_seen = false;
    s->starved = false;
    s->primed = false;
}

static bool af_all_zero(const float *frames, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (frames[i] != 0.0f) return false;
    }
    return true;
}

/* Opens `path` into a sound of its own, so that a failure leaves the sound
 * already playing untouched. */
static af_status af_sound_open(af_stream *s, const char *path, bool looping,
                               ma_sound **out)
{
    ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
    ma_sound *sound;
    ma_result result;

    if (looping) flags |= MA_SOUND_FLAG_LOOPING;

    sound = (ma_sound *)calloc(1, sizeof(*sound));
    if (sound == NULL) return AF_ERR_MEMORY;

    result = ma_sound_init_from_file(&s->engine, path, flags, NULL, NULL, sound);
    if (result != MA_SUCCESS) {
        /* Nothing was initialised, so nothing may be uninitialised. */
        free(sound);
        return af_status_from_ma(result);
    }

    *out = sound;
    return AF_OK;
}

af_status af_stream_open(af_stream *s, const char *path, af_info *info)
{
    af_info    file_info;
    af_status  status;
    ma_sound  *sound = NULL;
    ma_uint64  length = 0;
    char      *path_copy;

    if (s == NULL || path == NULL) return AF_ERR_ARGS;
    if (!s->engine_ready) return AF_ERR_CONFIG;

    /* Probed separately so that the reported info describes the file. */
    status = af_probe(path, &file_info);
    if (status != AF_OK) return status;

    status = af_sound_open(s, path, true, &sound);
    if (status != AF_OK) return status;

    ma_sound_get_length_in_pcm_frames(sound, &length);
    if (length == 0) {
        /* Nothing to count down to, so the end has to come from the decoder. */
        const bool looping = s->loop;

        af_free_sound(sound);
        sound  = NULL;
        status = af_sound_open(s, path, looping, &sound);
        if (status != AF_OK) return status;
    }
    ma_sound_set_pitch(sound, (float)s->speed);

    /* The path is what a sample-rate change reopens from, so a stream that
     * cannot remember it is not open. */
    path_copy = af_copy_string(path);
    if (path_copy == NULL) {
        af_free_sound(sound);
        return AF_ERR_MEMORY;
    }

    s->running = false;
    s->live = false;
    af_retire_sound(s);

    s->sound = sound;
    s->info  = file_info;
    s->engine_loops = length > 0;
    s->length = (uint64_t)length;
    free(s->path);
    s->path = path_copy;
    af_forget_position(s, 0.0);

    s->live = true;

    if (info != NULL) *info = file_info;
    return AF_OK;
}

af_status af_stream_info(af_stream *s, af_info *info)
{
    if (s == NULL || info == NULL) return AF_ERR_ARGS;
    if (s->sound == NULL) return AF_ERR_NOFILE;

    *info = s->info;
    return AF_OK;
}

void af_stream_close(af_stream *s)
{
    if (s == NULL) return;

    s->running = false;
    s->live = false;
    af_retire_sound(s);

    s->info = af_info_none;
    s->length = 0;
    s->engine_loops = false;
    free(s->path);
    s->path = NULL;
    af_forget_position(s, 0.0);
}

void af_stream_set_running(af_stream *s, int running)
{
    if (s == NULL) return;

    s->running = running != 0;
    if (s->sound == NULL) return;

    /* Starting a sound that reached the end rewinds it, which is not what
     * `run 1` after an eof means. */
    if (running != 0) {
        if (!ma_sound_at_end(s->sound)) ma_sound_start(s->sound);
    } else {
        ma_sound_stop(s->sound);
    }
}

void af_stream_set_loop(af_stream *s, int loop)
{
    if (s == NULL) return;

    s->loop = loop != 0;
    if (s->sound != NULL && !s->engine_loops) {
        ma_sound_set_looping(s->sound, (loop != 0) ? MA_TRUE : MA_FALSE);
    }
}

void af_stream_set_speed(af_stream *s, double speed)
{
    if (s == NULL || !(speed > 0.0)) return;
    if (speed < AF_SPEED_MIN) speed = AF_SPEED_MIN;
    if (speed > AF_SPEED_MAX) speed = AF_SPEED_MAX;

    s->speed = speed;
    if (s->sound != NULL) ma_sound_set_pitch(s->sound, (float)speed);
}

/* The engine's sample rate is fixed when it is created, so a change of Pd's
 * rate rebuilds it and reopens the file where it was. */
void af_stream_set_output_samplerate(af_stream *s, double samplerate)
{
    char    *path;
    uint64_t position;
    bool     was_running;

    if (s == NULL || !(samplerate > 0.0)) return;
    if ((uint32_t)(samplerate + 0.5) == (uint32_t)(s->out_rate + 0.5)) return;

    position    = af_stream_tell_frames(s);
    was_running = s->running;
    path        = s->path;
    s->path     = NULL;         /* the reopen below takes its own copy */

    af_engine_stop(s);
    s->live = false;
    s->length = 0;
    s->info = af_info_none;

    s->out_rate = samplerate;
    if (af_engine_start(s) == AF_OK && path != NULL &&
        af_stream_open(s, path, NULL) == AF_OK)
    {
        af_stream_seek_frames(s, position);
        af_stream_set_running(s, was_running ? 1 : 0);
        free(path);
        return;
    }

    /* Out of memory rebuilding the engine, or reopening the file into it.
     * There is nothing to report through: the object is silent until the next
     * open, which the path kept here lets a later rate change attempt. */
    free(s->path);
    s->path = path;
}

af_status af_stream_seek_frames(af_stream *s, uint64_t frame)
{
    uint64_t target, length;

    if (s == NULL) return AF_ERR_ARGS;
    if (s->sound == NULL) return AF_ERR_NOFILE;

    if (s->info.frames > 0 && frame > s->info.frames) frame = s->info.frames;
    target = af_to_engine_frames(s, frame);
    length = s->length;
    if (length > 0 && target > length) target = length;

    /* A sound at the end ignores a seek. Starting it clears that, at the cost
     * of a rewind the seek below replaces. */
    if (ma_sound_at_end(s->sound)) {
        ma_sound_start(s->sound);
        if (!s->running) ma_sound_stop(s->sound);
    }

    if (ma_sound_seek_to_pcm_frame(s->sound, target) != MA_SUCCESS) {
        return AF_ERR_SEEK;
    }
    af_forget_position(s, (double)target);
    s->settling = true;

    return AF_OK;
}

af_status af_stream_seek_seconds(af_stream *s, double seconds)
{
    double rate;
    double frame;

    if (s == NULL) return AF_ERR_ARGS;
    if (seconds < 0.0) seconds = 0.0;

    rate = s->info.samplerate;
    if (rate <= 0.0) return AF_ERR_NOFILE;

    frame = seconds * rate;
    if (frame > 1.8e19) frame = 1.8e19;

    return af_stream_seek_frames(s, (uint64_t)frame);
}

uint64_t af_stream_tell_frames(af_stream *s)
{
    if (s == NULL || !s->live) return 0;
    return af_to_file_frames(s, (uint64_t)(s->position + 0.5));
}

double af_stream_tell_seconds(af_stream *s)
{
    if (s == NULL || !s->live) return 0.0;
    if (s->out_rate <= 0.0) return 0.0;
    return s->position / s->out_rate;
}

int af_stream_pending(af_stream *s)
{
    return (s != NULL && s->events != 0) ? 1 : 0;
}

unsigned af_stream_take_events(af_stream *s)
{
    unsigned events;

    if (s == NULL) return 0u;
    events = s->events;
    s->events = 0u;
    return events;
}

static void af_raise(af_stream *s, unsigned event)
{
    s->events |= event;
}

static void af_report_eof(af_stream *s)
{
    if (!s->eof_seen) { s->eof_seen = true; af_raise(s, AF_EVENT_EOF); }
}

/* An empty buffer before the stream has filled after a seek is the buffer
 * filling, which is not a dropout. */
static void af_report_underflow(af_stream *s)
{
    if (s->primed && !s->starved) {
        s->starved = true;
        af_raise(s, AF_EVENT_UNDERFLOW);
    }
}

static size_t af_read_sound(af_stream *s, ma_sound *sound, float *dst, size_t frames)
{
    double    per_out, position;
    uint64_t  length, available;
    size_t    want;
    bool      at_end = false;
    ma_uint64 read = 0;

    if (sound == NULL) return 0;

    if (ma_sound_at_end(sound)) {
        af_report_eof(s);
        return 0;
    }

    /* Each output frame consumes `speed` frames of the file. */
    per_out = (double)ma_sound_get_pitch(sound);
    if (per_out <= 0.0) per_out = 1.0;

    position = s->position;
    length   = s->length;
    want     = frames;

    if (!s->loop && length > 0) {
        double remaining = (double)length - position;
        double out_left  = (remaining > 0.0) ? remaining / per_out : 0.0;

        if (out_left <= (double)want) {
            want   = (size_t)out_left;
            at_end = true;
        }
    }

    /* What is buffered bounds the block, so a starved stream is a dropout
     * rather than a stall. */
    available = af_available(sound);
    if (!at_end) {
        double servable = (double)available / per_out;
        if (servable < (double)want) want = (size_t)servable;
    }

    if (want > 0) {
        const uint64_t before   = af_source_cursor(sound);
        const bool     settling = s->settling;

        s->settling = false;

        ma_engine_read_pcm_frames(&s->engine, dst, want, &read);

        /* Silence the file did not produce is a gap: reporting it as no
         * frames stops the position advancing over frames nobody heard.
         * Silence in the file moves the cursor like anything else. */
        if (read > 0 && (settling || af_source_cursor(sound) == before) &&
            af_all_zero(dst, (size_t)read * s->channels))
        {
            return 0;
        }
    }

    if (read == frames) s->primed = true;

    position += (double)read * per_out;
    if (s->loop && length > 0) {
        while (position >= (double)length) position -= (double)length;
    }

    if (read < frames) {
        if (at_end || ma_sound_at_end(sound)) {
            /* The data source loops, so the play head has to be put back on
             * the end it just passed. */
            if (at_end && s->engine_loops) {
                position = (double)length;
                ma_sound_seek_to_pcm_frame(sound, length);
            }
            af_report_eof(s);
        } else {
            af_report_underflow(s);
        }
    } else {
        s->starved = false;
    }

    s->position = position;
    return (size_t)read;
}

size_t af_stream_read(af_stream *s, float *dst, size_t frames)
{
    if (s == NULL || dst == NULL || frames == 0) return 0;
    if (!s->live || !s->running) return 0;

    return af_read_sound(s, s->sound, dst, frames);
}
