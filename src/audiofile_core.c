/* audiofile_core.c -- one resource manager per stream, and two voices on it,
 * each an ma_engine in no-device mode playing one ma_sound of the open file.
 * One voice plays; the other prepares the place a cue moves to. The header
 * has the thread contract, the README the buffering.
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

/* More than enough for a block of 8 channels floating point audio */
#define AF_MINIAUDIO_STACK_BYTES 16384

#define AF_FALLBACK_SAMPLERATE 44100.0

/* The first frame out of a reset pitch resampler is its history, and a seek
 * restarts the decoder's resampler, so a cue reads this much and drops it
 * where the file has room before the pre-roll. */
#define AF_CUE_WARMUP_FRAMES 128

/* Output frame j after a sound seeks carries source frame j * speed - 1: the
 * first is the reset resampler's history. */
#define AF_RESAMPLER_DELAY 1

#define AF_CUE_PROBE_FRAMES 64

static const af_info af_info_none = { 0.0, 0.0, 0, "unknown" };

typedef struct {
    ma_engine engine;
    bool      engine_ready;
    ma_sound *sound;
} af_voice;

/* Frame counts are output frames, except `start`, a decoded frame. The idle
 * voice delivers `reads` frames: `discard`, then the pre-roll after its
 * `zeros`. `got` of them are in the stream's cue buffer. */
typedef struct {
    bool     waiting;
    bool     ready;
    double   seconds;
    double   t0;
    uint64_t zeros;
    uint64_t discard;
    uint64_t frames;
    uint64_t reads;
    uint64_t got;
    uint64_t start;
} af_cue;

struct af_stream {
    uint32_t channels;
    double   out_rate;
    double   speed;

    ma_resource_manager manager;
    bool                manager_ready;

    af_voice voices[2];
    unsigned current;

    char    *path;
    af_info  info;

    /* Seconds */
    double duration;
    double position;

    bool playing;
    bool looping;
    bool autorestart;
    bool seek_pending;

    bool eof_reported;
    bool underflow_reported;
    bool buffer_ran_once;

    af_cue cue;
    float *cue_buffer;
    size_t cue_capacity;

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
    out->duration   = (double)frames / (double)samplerate;
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

static ma_sound *af_sound(const af_stream *s)
{
    return s->voices[s->current].sound;
}

static af_voice *af_idle_voice(af_stream *s)
{
    return &s->voices[s->current ^ 1u];
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

static af_status af_manager_start(af_stream *s)
{
    ma_resource_manager_config config = ma_resource_manager_config_init();

    config.decodedFormat     = ma_format_f32;
    config.decodedChannels   = 0;
    config.decodedSampleRate = (ma_uint32)(s->out_rate + 0.5);
    config.resampling        = ma_engine_config_init().resourceManagerResampling;
#if defined(MA_EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
    config.jobThreadCount = 0;
    config.flags         |= MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
#endif

    if (ma_resource_manager_init(&config, &s->manager) != MA_SUCCESS) {
        return AF_ERR_MEMORY;
    }
    s->manager_ready = true;
    return AF_OK;
}

static af_status af_voice_start(af_stream *s, af_voice *voice)
{
    ma_engine_config config = ma_engine_config_init();

    if (voice->engine_ready) return AF_OK;
    if (config.listenerCount != 1) return AF_ERR_CONFIG;

    config.pResourceManager       = &s->manager;
    config.noDevice               = MA_TRUE;
    config.channels               = s->channels;
    config.sampleRate             = (ma_uint32)(s->out_rate + 0.5);
    config.preMixStackSizeInBytes = AF_MINIAUDIO_STACK_BYTES;

    /* No fixed period: with one, the node graph keeps the rest of a period
     * after a shorter read, and a voice would carry it into its next cue. */
    config.periodSizeInFrames = 0;

    if (ma_engine_init(&config, &voice->engine) != MA_SUCCESS) return AF_ERR_MEMORY;
    voice->engine_ready = true;
    return AF_OK;
}

static void af_free_sound(ma_sound *sound)
{
    if (sound == NULL) return;
    ma_sound_uninit(sound);
    free(sound);
}

static void af_voice_release(af_voice *voice)
{
    af_free_sound(voice->sound);
    voice->sound = NULL;
}

static af_status af_engine_start(af_stream *s)
{
    af_status status = af_manager_start(s);

    if (status != AF_OK) return status;
    s->current = 0;
    status = af_voice_start(s, &s->voices[0]);
    if (status != AF_OK) return status;
    return af_voice_start(s, &s->voices[1]);
}

static void af_engine_stop(af_stream *s)
{
    unsigned i;

    for (i = 0; i < 2; i++) {
        af_voice_release(&s->voices[i]);
        if (s->voices[i].engine_ready) {
            ma_engine_uninit(&s->voices[i].engine);
            s->voices[i].engine_ready = false;
        }
    }
    if (s->manager_ready) {
        ma_resource_manager_uninit(&s->manager);
        s->manager_ready = false;
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

    s->channels    = channels;
    s->out_rate    = out_samplerate;
    s->info        = af_info_none;
    s->speed       = 1.0;
    s->autorestart = true;

    status = af_engine_start(s);
    if (status != AF_OK) {
        af_engine_stop(s);
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
    free(s->cue_buffer);
    free(s->path);
    free(s);
}

static void af_clear_reports(af_stream *s)
{
    s->eof_reported       = false;
    s->underflow_reported = false;
    s->buffer_ran_once    = false;
}

static void af_cancel_cue(af_stream *s)
{
    memset(&s->cue, 0, sizeof(s->cue));
}

static bool af_all_zero(const float *frames, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (frames[i] != 0.0f) return false;
    }
    return true;
}

static af_status af_sound_open(af_voice *voice, const char *path, bool looping,
                               ma_sound **out)
{
    ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
    ma_sound *sound;
    ma_result result;

    if (looping) flags |= MA_SOUND_FLAG_LOOPING;

    sound = (ma_sound *)calloc(1, sizeof(*sound));
    if (sound == NULL) return AF_ERR_MEMORY;

    result = ma_sound_init_from_file(&voice->engine, path, flags, NULL, NULL, sound);
    if (result != MA_SUCCESS) {
        free(sound);
        return af_status_from_ma(result);
    }

    *out = sound;
    return AF_OK;
}

/* A file that records no length has its end found by the decoder, so only
 * that one plays with the stream's own looping. */
static af_status af_sound_load(af_stream *s, af_voice *voice, const char *path,
                               ma_sound **out, ma_uint64 *length)
{
    ma_sound *sound = NULL;
    af_status status;

    *length = 0;
    status  = af_sound_open(voice, path, true, &sound);
    if (status != AF_OK) return status;

    ma_sound_get_length_in_pcm_frames(sound, length);
    if (*length == 0) {
        af_free_sound(sound);
        sound  = NULL;
        status = af_sound_open(voice, path, s->looping, &sound);
        if (status != AF_OK) return status;
    }
    ma_sound_set_pitch(sound, (float)s->speed);

    *out = sound;
    return AF_OK;
}

af_status af_stream_open(af_stream *s, const char *path, af_info *info)
{
    af_voice  *voice, *idle;
    af_info    file_info;
    af_status  status;
    ma_sound  *sound = NULL, *idle_sound = NULL;
    ma_uint64  length = 0, idle_length = 0;
    char      *path_copy;

    if (s == NULL || path == NULL) return AF_ERR_ARGS;
    if (!s->manager_ready) return AF_ERR_CONFIG;

    voice = &s->voices[s->current];
    idle  = af_idle_voice(s);

    status = af_probe(path, &file_info);
    if (status != AF_OK) return status;

    status = af_sound_load(s, voice, path, &sound, &length);
    if (status != AF_OK) return status;

    status = af_sound_load(s, idle, path, &idle_sound, &idle_length);
    path_copy = (status == AF_OK) ? af_copy_string(path) : NULL;
    if (status == AF_OK && path_copy == NULL) status = AF_ERR_MEMORY;
    if (status != AF_OK) {
        af_free_sound(idle_sound);
        af_free_sound(sound);
        return status;
    }

    s->playing = false;
    af_cancel_cue(s);
    af_voice_release(idle);
    af_voice_release(voice);

    voice->sound     = sound;
    idle->sound      = idle_sound;
    s->info          = file_info;
    s->duration = (double)length / s->out_rate;
    free(s->path);
    s->path = path_copy;
    af_clear_reports(s);
    s->position = 0.0;
    s->seek_pending = false;

    if (info != NULL) *info = file_info;
    return AF_OK;
}

af_status af_stream_info(af_stream *s, af_info *info)
{
    if (s == NULL || info == NULL) return AF_ERR_ARGS;
    if (af_sound(s) == NULL) return AF_ERR_NOFILE;

    *info = s->info;
    return AF_OK;
}

void af_stream_close(af_stream *s)
{
    if (s == NULL) return;

    s->playing = false;
    af_cancel_cue(s);
    af_voice_release(&s->voices[0]);
    af_voice_release(&s->voices[1]);

    s->info     = af_info_none;
    s->duration = 0.0;
    free(s->path);
    s->path = NULL;
    af_clear_reports(s);
    s->position = 0.0;
    s->seek_pending = false;
}

static bool af_at_end(af_stream *s)
{
    ma_sound *sound = af_sound(s);

    if (sound == NULL) return false;
    if (ma_sound_at_end(sound)) return true;

    return s->duration > 0.0 && s->position >= s->duration;
}

void af_stream_set_playing(af_stream *s, int playing)
{
    ma_sound *sound;

    if (s == NULL) return;

    s->playing = playing;
    sound = af_sound(s);
    if (sound == NULL) return;

    if (playing) {
        if (s->autorestart && af_at_end(s)) af_stream_seek_seconds(s, 0.0);
        if (!ma_sound_at_end(sound)) ma_sound_start(sound);
    } else {
        ma_sound_stop(sound);
    }
}

void af_stream_set_autorestart(af_stream *s, int autorestart)
{
    if (s == NULL) return;

    s->autorestart = autorestart != 0;
}

void af_stream_set_looping(af_stream *s, int looping)
{
    unsigned i;

    if (s == NULL) return;

    s->looping = looping;
    if (s->duration != 0.0) return;

    for (i = 0; i < 2; i++) {
        if (s->voices[i].sound != NULL) {
            ma_sound_set_looping(s->voices[i].sound, looping ? MA_TRUE : MA_FALSE);
        }
    }
}

void af_stream_set_speed(af_stream *s, double speed)
{
    unsigned i;

    if (s == NULL || !(speed > 0.0)) return;
    if (speed < AF_SPEED_MIN) speed = AF_SPEED_MIN;
    if (speed > AF_SPEED_MAX) speed = AF_SPEED_MAX;

    s->speed = speed;
    for (i = 0; i < 2; i++) {
        if (s->voices[i].sound != NULL) {
            ma_sound_set_pitch(s->voices[i].sound, (float)speed);
        }
    }
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

    af_cancel_cue(s);
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

static double af_clamp_seconds(const af_stream *s, double seconds)
{
    if (!(seconds > 0.0)) seconds = 0.0;
    if (s->duration > 0.0 && seconds > s->duration) seconds = s->duration;
    return seconds;
}

af_status af_stream_seek_seconds(af_stream *s, double seconds)
{
    ma_sound *sound;

    if (s == NULL) return AF_ERR_ARGS;
    sound = af_sound(s);
    if (sound == NULL) return AF_ERR_NOFILE;

    seconds = af_clamp_seconds(s, seconds);
    af_cancel_cue(s);

    if (ma_sound_at_end(sound)) {
        ma_sound_start(sound);
        if (!s->playing) ma_sound_stop(sound);
    }

    if (ma_sound_seek_to_pcm_frame(sound, af_seconds_to_frames(s, seconds)) != MA_SUCCESS) {
        return AF_ERR_SEEK;
    }
    af_clear_reports(s);
    s->position = seconds;
    s->seek_pending = true;

    return AF_OK;
}

af_status af_stream_cue(af_stream *s, double seconds, double preroll)
{
    af_voice *idle;
    double    speed;
    uint64_t  cue_frame, room, source;
    size_t    samples;
    af_cue    cue;

    if (s == NULL) return AF_ERR_ARGS;
    idle = af_idle_voice(s);
    if (af_sound(s) == NULL || idle->sound == NULL) return AF_ERR_NOFILE;

    speed = s->speed;
    if (!(preroll >= 0.0) || preroll > AF_CUE_PREROLL_MAX ||
        preroll / speed > AF_CUE_PREROLL_MAX)
    {
        return AF_ERR_ARGS;
    }

    af_cancel_cue(s);
    seconds = af_clamp_seconds(s, seconds);

    memset(&cue, 0, sizeof(cue));
    cue_frame  = af_seconds_to_frames(s, seconds);
    cue.frames = (uint64_t)(preroll * s->out_rate / speed + 0.5);

    /* Output frames that can be read before the cue point from the file's
     * start, the resampler's history included. */
    room = (uint64_t)((double)(cue_frame + AF_RESAMPLER_DELAY) / speed);

    if (room >= cue.frames + AF_CUE_WARMUP_FRAMES) {
        cue.discard = AF_CUE_WARMUP_FRAMES;
    } else if (room >= cue.frames) {
        cue.discard = room - cue.frames;
    } else {
        cue.zeros = cue.frames - room;
    }

    cue.reads   = cue.discard + cue.frames - cue.zeros;
    source      = (uint64_t)((double)cue.reads * speed + 0.5);
    cue.start   = (source <= cue_frame + AF_RESAMPLER_DELAY)
                ? cue_frame + AF_RESAMPLER_DELAY - source : 0;
    cue.seconds = seconds;
    cue.t0      = seconds - (double)cue.frames * speed / s->out_rate;

    samples = (size_t)cue.reads * s->channels;
    if (samples > s->cue_capacity) {
        float *grown = (float *)realloc(s->cue_buffer, samples * sizeof(float));
        if (grown == NULL) return AF_ERR_MEMORY;
        s->cue_buffer   = grown;
        s->cue_capacity = samples;
    }

    /* The seek, and the reset of the sound's read-ahead and resampler that
     * comes with it, happen when the idle engine is next read. */
    ma_sound_start(idle->sound);
    if (ma_sound_seek_to_pcm_frame(idle->sound, cue.start) != MA_SUCCESS) {
        return AF_ERR_SEEK;
    }

    cue.waiting = true;
    s->cue = cue;
    return AF_OK;
}

size_t af_stream_cue_frames(af_stream *s)
{
    if (s == NULL || !(s->cue.waiting || s->cue.ready)) return 0;
    return (size_t)s->cue.frames;
}

af_status af_stream_take_cue(af_stream *s, float *dst, double *t0,
                             double *seconds)
{
    af_voice *idle, *playing;
    af_status status = AF_OK;
    ma_uint64 read = 0;
    uint64_t  rest;
    size_t    channels;

    if (s == NULL) return AF_ERR_ARGS;
    if (!s->cue.ready) return AF_ERR_ARGS;

    playing  = &s->voices[s->current];
    idle     = af_idle_voice(s);
    channels = s->channels;
    rest     = s->cue.reads - s->cue.got;

    if (rest > 0) {
        float *to = s->cue_buffer + s->cue.got * channels;

        ma_engine_read_pcm_frames(&idle->engine, to, rest, &read);
        if (read < rest) {
            memset(to + read * channels, 0, (size_t)(rest - read) * channels * sizeof(float));
            status = AF_ERR_SEEK;
        }
    }

    if (dst != NULL) {
        size_t zero_samples = (size_t)s->cue.zeros * channels;

        memset(dst, 0, zero_samples * sizeof(float));
        memcpy(dst + zero_samples, s->cue_buffer + s->cue.discard * channels,
               (size_t)(s->cue.reads - s->cue.discard) * channels * sizeof(float));
    }

    if (!s->playing) ma_sound_stop(idle->sound);
    if (playing->sound != NULL) ma_sound_stop(playing->sound);
    s->current = s->current ^ 1u;

    if (t0 != NULL) *t0 = s->cue.t0;
    if (seconds != NULL) *seconds = s->cue.seconds;

    af_clear_reports(s);
    s->position     = s->cue.seconds;
    s->seek_pending = false;
    af_cancel_cue(s);
    return status;
}

double af_stream_tell_seconds(af_stream *s)
{
    if (s == NULL || af_sound(s) == NULL) return 0.0;
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

/* A stream answers reads with nothing until its seek has landed, so the
 * cursor stays at the cue's start and what the engine gives back is silence.
 * Once it moves, the frames read are the first of the cue. */
static void af_poll_cue(af_stream *s)
{
    af_voice *idle = af_idle_voice(s);
    ma_uint64 read = 0, cursor = 0;
    uint64_t  want;

    if (!s->cue.waiting) return;

    want = (s->cue.reads < AF_CUE_PROBE_FRAMES) ? s->cue.reads : AF_CUE_PROBE_FRAMES;
    ma_engine_read_pcm_frames(&idle->engine, s->cue_buffer, want, &read);
    ma_data_source_get_cursor_in_pcm_frames(ma_sound_get_data_source(idle->sound), &cursor);
    if (cursor == s->cue.start) return;

    s->cue.got     = read;
    s->cue.waiting = false;
    s->cue.ready   = true;
    af_raise(s, AF_EVENT_CUED);
}

size_t af_stream_read(af_stream *s, float *dst, size_t frames)
{
    ma_sound *sound;
    ma_engine *engine;
    double    speed, position, block_seconds;
    uint64_t  buffered;
    size_t    to_read;
    bool      at_end = false;
    ma_uint64 read = 0;

    if (s == NULL || dst == NULL || frames == 0) return 0;

    af_poll_cue(s);

    sound  = af_sound(s);
    engine = &s->voices[s->current].engine;
    if (sound == NULL || !s->playing) return 0;

    if (ma_sound_at_end(sound)) {
        af_report_eof(s);
        return 0;
    }

    /* One output frame carries `speed` seconds of the file. Engine rate. */
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
        const uint64_t prev_frames_read = af_frames_read(sound);
        const bool     seek_pending     = s->seek_pending;

        s->seek_pending = false;

        ma_engine_read_pcm_frames(engine, dst, to_read, &read);

        /* Nothing to play. Return early. */
        if (read > 0 && (seek_pending || af_frames_read(sound) == prev_frames_read) &&
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
