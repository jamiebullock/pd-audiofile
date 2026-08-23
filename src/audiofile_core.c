/* audiofile_core.c -- one ma_engine per stream, in no-device mode, and one
 * ma_sound per open file. The header has the thread contract, the README the
 * buffering.
 *
 * Part of pd-audiofile
 *
 * SPDX-FileCopyrightText: 2026 Jamie Bullock
 * SPDX-License-Identifier: Zlib
 */

#include "audiofile_core.h"
#include "af_miniaudio.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Set to the default Pd Blocksize */
#define AF_MINIAUDIO_PERIOD_FRAMES 64

/* More than enough for a block of 8 channels floating point audio */
#define AF_MINIAUDIO_STACK_BYTES 16384

#define AF_FALLBACK_SAMPLERATE 44100.0

static const af_info af_info_none = { 0.0, 0, 0, "unknown" };

struct af_stream {
    uint32_t channels;
    double   out_rate;
    double   speed;

    ma_engine engine;
    bool      engine_ready;

    ma_sound *sound;

    char    *path;
    af_info  info;

    /* Seconds */
    double duration;
    double position;

    bool playing;
    bool looping;
    bool seek_pending;

    bool eof_reported;
    bool underflow_reported;
    bool buffer_ran_once;

    unsigned pending_events;
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

static uint64_t af_seconds_to_frames(const af_stream *s, double seconds)
{
    return (uint64_t)(seconds * s->out_rate + 0.5);
}

static uint64_t af_frames_read(ma_sound *sound)
{
    ma_uint64 frames = 0;
    ma_data_source *source = (sound != NULL) ? ma_sound_get_data_source(sound) : NULL;

    if (source == NULL) return 0;
    ma_data_source_get_cursor_in_pcm_frames(source, &frames);
    return (uint64_t)frames;
}

static uint64_t af_frames_buffered(ma_sound *sound)
{
    ma_uint64 frames = 0;
    ma_data_source *source = (sound != NULL) ? ma_sound_get_data_source(sound) : NULL;

    if (source == NULL) return 0;
    ma_resource_manager_data_source_get_available_frames(
        (ma_resource_manager_data_source *)source, &frames);
    return (uint64_t)frames;
}

static af_status af_engine_start(af_stream *s)
{
    ma_engine_config config = ma_engine_config_init();

    if (config.listenerCount != 1) return AF_ERR_CONFIG;

    config.noDevice               = MA_TRUE;
    config.channels               = s->channels;
    config.sampleRate             = (ma_uint32)(s->out_rate + 0.5);
    config.periodSizeInFrames     = AF_MINIAUDIO_PERIOD_FRAMES;
    config.preMixStackSizeInBytes = AF_MINIAUDIO_STACK_BYTES;

    if (ma_engine_init(&config, &s->engine) != MA_SUCCESS) return AF_ERR_MEMORY;
    s->engine_ready = true;
    return AF_OK;
}

static void af_free_sound(ma_sound *sound)
{
    if (sound == NULL) return;
    ma_sound_uninit(sound);
    free(sound);
}

static void af_engine_stop(af_stream *s)
{
    af_free_sound(s->sound);
    s->sound = NULL;
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

static void af_clear_reports(af_stream *s)
{
    s->eof_reported       = false;
    s->underflow_reported = false;
    s->buffer_ran_once    = false;
}

static bool af_all_zero(const float *frames, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (frames[i] != 0.0f) return false;
    }
    return true;
}

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

    status = af_probe(path, &file_info);
    if (status != AF_OK) return status;

    status = af_sound_open(s, path, true, &sound);
    if (status != AF_OK) return status;

    ma_sound_get_length_in_pcm_frames(sound, &length);
    if (length == 0) {
        /* No length in the file so the end has to come from the decoder. */
        const bool looping = s->looping;

        af_free_sound(sound);
        sound  = NULL;
        status = af_sound_open(s, path, looping, &sound);
        if (status != AF_OK) return status;
    }
    ma_sound_set_pitch(sound, (float)s->speed);

    path_copy = af_copy_string(path);
    if (path_copy == NULL) {
        af_free_sound(sound);
        return AF_ERR_MEMORY;
    }

    s->playing = false;
    af_free_sound(s->sound);

    s->sound         = sound;
    s->info          = file_info;
    s->duration = (double)length / s->out_rate;
    free(s->path);
    s->path = path_copy;
    af_clear_reports(s);
    s->position = 0.0;

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

    s->playing = false;
    af_free_sound(s->sound);
    s->sound = NULL;

    s->info     = af_info_none;
    s->duration = 0.0;
    free(s->path);
    s->path = NULL;
    af_clear_reports(s);
    s->position = 0.0;
}

void af_stream_set_playing(af_stream *s, int playing)
{
    if (s == NULL) return;

    s->playing = playing;
    if (s->sound == NULL) return;

    if (playing) {
        if (!ma_sound_at_end(s->sound)) ma_sound_start(s->sound);
    } else {
        ma_sound_stop(s->sound);
    }
}

void af_stream_set_looping(af_stream *s, int looping)
{
    if (s == NULL) return;

    s->looping = looping;
    if (s->sound != NULL && s->duration == 0.0) {
        ma_sound_set_looping(s->sound, looping ? MA_TRUE : MA_FALSE);
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

void af_stream_set_output_samplerate(af_stream *s, double samplerate)
{
    char  *path;
    double position;
    bool   was_playing;

    if (s == NULL || !(samplerate > 0.0)) return;
    if ((uint32_t)(samplerate + 0.5) == (uint32_t)(s->out_rate + 0.5)) return;

    position    = s->position;
    was_playing = s->playing;
    path        = s->path;
    s->path     = NULL;

    af_engine_stop(s);
    s->duration = 0.0;
    s->info     = af_info_none;

    s->out_rate = samplerate;
    if (af_engine_start(s) == AF_OK && path != NULL &&
        af_stream_open(s, path, NULL) == AF_OK)
    {
        af_stream_seek_seconds(s, position);
        af_stream_set_playing(s, was_playing ? 1 : 0);
        free(path);
        return;
    }

    free(s->path);
    s->path = path;
}

af_status af_stream_seek_seconds(af_stream *s, double seconds)
{
    if (s == NULL) return AF_ERR_ARGS;
    if (s->sound == NULL) return AF_ERR_NOFILE;

    if (!(seconds > 0.0)) seconds = 0.0;
    if (s->duration > 0.0 && seconds > s->duration) seconds = s->duration;

    if (ma_sound_at_end(s->sound)) {
        ma_sound_start(s->sound);
        if (!s->playing) ma_sound_stop(s->sound);
    }

    if (ma_sound_seek_to_pcm_frame(s->sound, af_seconds_to_frames(s, seconds)) != MA_SUCCESS) {
        return AF_ERR_SEEK;
    }
    af_clear_reports(s);
    s->position = seconds;
    s->seek_pending = true;

    return AF_OK;
}

double af_stream_tell_seconds(af_stream *s)
{
    if (s == NULL || s->sound == NULL) return 0.0;
    return s->position;
}

int af_stream_pending(af_stream *s)
{
    return (s != NULL && s->pending_events != 0) ? 1 : 0;
}

unsigned af_stream_read_events(af_stream *s)
{
    unsigned events;

    if (s == NULL) return 0u;
    events = s->pending_events;
    s->pending_events = 0u;
    return events;
}

static void af_raise(af_stream *s, unsigned event)
{
    s->pending_events |= event;
}

static void af_report_eof(af_stream *s)
{
    if (!s->eof_reported) { s->eof_reported = true; af_raise(s, AF_EVENT_EOF); }
}

static void af_report_underflow(af_stream *s)
{
    if (s->buffer_ran_once && !s->underflow_reported) {
        s->underflow_reported = true;
        af_raise(s, AF_EVENT_UNDERFLOW);
    }
}

size_t af_stream_read(af_stream *s, float *dst, size_t frames)
{
    ma_sound *sound;
    double    speed, position, block_seconds;
    uint64_t  buffered;
    size_t    to_read;
    bool      at_end = false;
    ma_uint64 read = 0;

    if (s == NULL || dst == NULL || frames == 0) return 0;
    if (s->sound == NULL || !s->playing) return 0;

    sound = s->sound;

    if (ma_sound_at_end(sound)) {
        af_report_eof(s);
        return 0;
    }

    /* One output frame carries `speed` seconds of the file, at the engine's
     * rate. */
    speed = (double)ma_sound_get_pitch(sound);
    if (speed <= 0.0) speed = 1.0;
    block_seconds = speed / s->out_rate;

    position = s->position;
    to_read  = frames;

    if (!s->looping && s->duration > 0.0) {
        double remaining = s->duration - position;
        double out_left  = (remaining > 0.0) ? remaining / block_seconds : 0.0;

        if (out_left <= (double)to_read) {
            to_read = (size_t)out_left;
            at_end  = true;
        }
    }

    buffered = af_frames_buffered(sound);
    if (!at_end) {
        double out_buffered = (double)buffered / speed;
        if (out_buffered < (double)to_read) to_read = (size_t)out_buffered;
    }

    if (to_read > 0) {
        const uint64_t before   = af_frames_read(sound);
        const bool     seek_pending = s->seek_pending;

        s->seek_pending = false;

        ma_engine_read_pcm_frames(&s->engine, dst, to_read, &read);

        /* Silence the file did not produce is a gap: reporting it as no
         * frames stops the position advancing over frames nobody heard.
         * Silence in the file moves the cursor like anything else. */
        if (read > 0 && (seek_pending || af_frames_read(sound) == before) &&
            af_all_zero(dst, (size_t)read * s->channels))
        {
            return 0;
        }
    }

    if (read == frames) s->buffer_ran_once = true;

    position += (double)read * block_seconds;
    if (s->looping && s->duration > 0.0) {
        while (position >= s->duration) position -= s->duration;
    }

    if (read < frames) {
        if (at_end || ma_sound_at_end(sound)) {
            /* The data source loops, so the play head has to be put back on
             * the end it just passed. */
            if (at_end) {
                position = s->duration;
                ma_sound_seek_to_pcm_frame(sound, af_seconds_to_frames(s, s->duration));
            }
            af_report_eof(s);
        } else {
            af_report_underflow(s);
        }
    } else {
        s->underflow_reported = false;
    }

    s->position = position;
    return (size_t)read;
}
