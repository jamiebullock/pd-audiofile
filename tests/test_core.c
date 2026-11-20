/* test_core.c -- audiofile_core with no Pd anywhere near it.
 *
 * tests/make_fixtures.py writes every fixture from one sine at 0.5, 100
 * frames per cycle at 44100 Hz: a signal known frame by frame, so a test can
 * say where a seek landed.
 *
 * Part of pd-audiofile
 *
 * SPDX-FileCopyrightText: 2026 Jamie Bullock
 * SPDX-License-Identifier: Zlib
 */

#include "audiofile_core.h"

#include "greatest.h"

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <process.h>
    #define af_process_id() ((long)_getpid())
#else
    #include <time.h>
    #include <unistd.h>
    #define af_process_id() ((long)getpid())
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FIXTURE_RATE    44100.0
#define FIXTURE_FRAMES  22050
#define FIXTURE_SECONDS (FIXTURE_FRAMES / FIXTURE_RATE)
#define FIXTURE_PERIOD  100.0
#define FIXTURE_AMP     0.5

/* M_PI is not in standard C. */
#define AF_PI           3.14159265358979323846

#if defined(_WIN32)

static void af_sleep_ms(int ms) { Sleep((DWORD)ms); }

#else

static void af_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

#endif

static const char *g_dir = "fixtures";

static char *fixture(const char *name)
{
    static char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    return path;
}

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

static const char *msg(const char *fmt, ...)
{
    static char text[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    return text;
}

/* ------------------------------------------------------------------ */
/* Reading                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t frames;
    int    eofs;
    int    timed_out;
} af_drain;

/* Waits when nothing is buffered, and gives up after `budget_ms`, so a stream
 * that stops reading ahead fails the test. */
static af_drain drain(af_stream *s, float *out, size_t limit,
                      size_t block, size_t channels, int budget_ms)
{
    af_drain d;
    int idle_ms = 0;

    memset(&d, 0, sizeof(d));

    while (d.frames < limit) {
        size_t want = limit - d.frames;
        size_t got;

        if (want > block) want = block;
        got = af_stream_read(s, out ? out + d.frames * channels : NULL, want);

        if (af_stream_read_events(s) & AF_EVENT_EOF) d.eofs++;

        if (got == 0) {
            if (d.eofs > 0) break;
            af_sleep_ms(1);
            if (++idle_ms > budget_ms) { d.timed_out = 1; break; }
            continue;
        }
        idle_ms = 0;
        d.frames += got;
    }
    return d;
}

/* Reads until a block comes back whole, which clears the stalled state, so a
 * stall raised after this is new. */
static int settle(af_stream *s, float *buf, size_t frames)
{
    int waited = 0;
    while (af_stream_read(s, buf, frames) < frames) {
        af_sleep_ms(1);
        if (++waited > 2000) return 0;
    }
    af_stream_read_events(s);
    return 1;
}

static float *scratch_buffer(size_t frames, size_t channels)
{
    float *p = (float *)calloc(frames * channels, sizeof(float));
    if (p == NULL) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

static uint64_t tell_frames(af_stream *s)
{
    af_info info;
    if (af_stream_info(s, &info) != AF_OK) return 0;
    return (uint64_t)(af_stream_tell_seconds(s) * info.samplerate + 0.5);
}

static double expected_sample(double frame)
{
    return FIXTURE_AMP * sin(2.0 * AF_PI * frame / FIXTURE_PERIOD);
}

/* The resampler holds a frame of history, so the block after a seek is the
 * file at the target give or take a small lag. Searching the lag range
 * separates landing in the wrong place from landing a frame early.
 *
 * `step` is how many file frames each output frame advances by. */
static double phase_error(const float *buf, size_t frames, double start_frame,
                          double step, int *best_lag)
{
    double best = 1e9;
    int lag;

    for (lag = -3; lag <= 3; lag++) {
        double worst = 0.0;
        size_t i;
        for (i = 0; i < frames; i++) {
            double want = expected_sample(start_frame + ((double)i + lag) * step);
            double err  = fabs((double)buf[i] - want);
            if (err > worst) worst = err;
        }
        if (worst < best) {
            best = worst;
            if (best_lag) *best_lag = lag;
        }
    }
    return best;
}

/* The first frames out of a reset resampler are its own history. */
#define AF_PRIME 8

/* Returns from the calling test if either step does not work. */
#define OPEN_FIXTURE(s, info, name, channels, rate)                          \
    do {                                                                     \
        ASSERTm("af_stream_new", af_stream_new(&(s), (channels), (rate)) == AF_OK); \
        ASSERTm("open", af_stream_open((s), fixture(name), &(info)) == AF_OK); \
    } while (0)

/* ------------------------------------------------------------------ */
/* Probing                                                            */
/* ------------------------------------------------------------------ */

TEST probe_reports_the_header(const char *name, double rate, double duration,
                              uint32_t channels, const char *format)
{
    af_info   info;
    af_status status;

    if (!exists(fixture(name))) SKIPm("fixture not generated");

    status = af_probe(fixture(name), &info);
    ASSERTm(msg("status %s", af_status_string(status)), status == AF_OK);

    ASSERTm(msg("samplerate %g, wanted %g", info.samplerate, rate),
            info.samplerate == rate);
    ASSERTm(msg("channels %u, wanted %u", info.channels, channels),
            info.channels == channels);

    /* Lossy encoders pad, so those fixtures are checked without a length. */
    if (duration > 0.0) {
        ASSERTm(msg("duration %g, wanted %g", info.duration, duration),
                fabs(info.duration - duration) < 1e-9);
    }
    if (format != NULL) {
        ASSERTm(msg("format %s, wanted %s", info.format, format),
                strcmp(info.format, format) == 0);
    }
    PASS();
}

TEST probe_on_a_file_that_is_not_there(void)
{
    af_info   info;
    af_status status = af_probe(fixture("no_such_file.wav"), &info);

    ASSERTm(msg("status %s, wanted nofile", af_status_string(status)),
            status == AF_ERR_OPEN);
    PASS();
}

TEST probe_on_a_malformed_file(void)
{
    af_info   info;
    af_status status = af_probe(fixture("garbage.wav"), &info);

    ASSERTm("a text file was accepted as audio", status != AF_OK);
    ASSERTm(msg("status %s, wanted format", af_status_string(status)),
            status == AF_ERR_FORMAT);
    PASS();
}

TEST probe_with_no_path(void)
{
    af_info   info;
    af_status status = af_probe(NULL, &info);

    ASSERTm(msg("status %s, wanted badargs", af_status_string(status)),
            status == AF_ERR_ARGS);
    PASS();
}

SUITE(probing)
{
    struct { const char *name; double rate; double duration;
             uint32_t channels; const char *format; } cases[] = {
        { "sine_mono_44100_s16.wav",   44100.0, FIXTURE_SECONDS, 1, "s16" },
        { "sine_mono_44100_f32.wav",   44100.0, FIXTURE_SECONDS, 1, "f32" },
        { "sine_stereo_44100_s16.wav", 44100.0, FIXTURE_SECONDS, 2, "s16" },
        { "sine_mono_48000_s16.wav",   48000.0, FIXTURE_FRAMES / 48000.0, 1, "s16" },
        { "sine_mono_44100_s16.aiff",  44100.0, FIXTURE_SECONDS, 1, "s16" },
        { "sine_mono_44100_s24.aiff",  44100.0, FIXTURE_SECONDS, 1, "s24" },
        { "trailing_chunk.wav",        44100.0, FIXTURE_SECONDS, 1, "s16" },
        { "sine_mono_44100.flac",      44100.0, FIXTURE_SECONDS, 1, NULL  },
        { "sine_mono_44100.mp3",       44100.0, 0.0,            1, NULL  },
        { "sine_mono_44100.ogg",       44100.0, 0.0,            1, NULL  }
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        greatest_set_test_suffix(cases[i].name);
        RUN_TESTp(probe_reports_the_header, cases[i].name, cases[i].rate,
                  cases[i].duration, cases[i].channels, cases[i].format);
    }

    RUN_TEST(probe_on_a_file_that_is_not_there);
    RUN_TEST(probe_on_a_malformed_file);
    RUN_TEST(probe_with_no_path);
}

/* ------------------------------------------------------------------ */
/* Streaming                                                          */
/* ------------------------------------------------------------------ */

TEST reads_end_to_end(const char *name, uint32_t channels)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;
    size_t     limit;

    if (!exists(fixture(name))) SKIPm("fixture not generated");

    OPEN_FIXTURE(s, info, name, channels, FIXTURE_RATE);

    limit = (size_t)FIXTURE_FRAMES * 2;
    buf = scratch_buffer(limit, channels);

    af_stream_set_playing(s, 1);
    d = drain(s, buf, limit, 64, channels, 5000);

    ASSERTm("timed out", !d.timed_out);
    ASSERTm(msg("eof reported %d times, wanted once", d.eofs), d.eofs == 1);

    /* One frame of resampler history, and MP3 adds encoder padding. */
    ASSERTm(msg("produced %lu frames, wanted %d",
                (unsigned long)d.frames, FIXTURE_FRAMES),
            labs((long)d.frames - (long)FIXTURE_FRAMES) <= 8);

    if (channels == 1) {
        int lag = 0;
        double err = phase_error(buf + AF_PRIME, 512, (double)AF_PRIME, 1.0, &lag);
        ASSERTm(msg("waveform is off by %.4f at lag %d", err, lag), err < 0.02);
    }

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_truncated_file_reads_short(void)
{
    af_info    info;
    af_status  status;
    af_stream *s;
    float     *buf;
    af_drain   d;

    status = af_probe(fixture("truncated.wav"), &info);
    ASSERTm(msg("status %s: the header describes the file it meant to be",
                af_status_string(status)),
            status == AF_OK);

    OPEN_FIXTURE(s, info, "truncated.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(FIXTURE_FRAMES * 2, 1);
    af_stream_set_playing(s, 1);
    d = drain(s, buf, FIXTURE_FRAMES * 2, 256, 1, 3000);

    ASSERTm("timed out draining a truncated file", !d.timed_out);
    ASSERTm(msg("read %lu frames from a file cut short of %d",
                (unsigned long)d.frames, FIXTURE_FRAMES),
            d.frames < (size_t)FIXTURE_FRAMES);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_stereo_file_keeps_its_channels_apart(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;

    OPEN_FIXTURE(s, info, "sine_stereo_44100_s16.wav", 2, FIXTURE_RATE);
    ASSERTm(msg("channels %u", info.channels), info.channels == 2);

    buf = scratch_buffer(4096, 2);
    af_stream_set_playing(s, 1);
    d = drain(s, buf, 1024, 64, 2, 3000);
    ASSERTm(msg("got %lu frames", (unsigned long)d.frames), d.frames == 1024);

    /* The fixture inverts its second channel. */
    {
        double worst = 0.0;
        size_t i;
        for (i = 8; i < 1024; i++) {
            double err = fabs((double)buf[i * 2] + (double)buf[i * 2 + 1]);
            if (err > worst) worst = err;
        }
        ASSERTm(msg("the two channels are not mirrored: off by %.4f", worst),
                worst < 0.01);
    }

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST info_describes_the_file_not_the_playback(void)
{
    af_stream *s;
    af_info    stream_info, probe_info;

    /* One channel out, two in, and the file's own rate unlike the output's. */
    OPEN_FIXTURE(s, stream_info, "sine_stereo_44100_s16.wav", 1, 22050.0);
    ASSERTm("probe",
            af_probe(fixture("sine_stereo_44100_s16.wav"), &probe_info) == AF_OK);

    ASSERTm(msg("channels: the stream says %u, the file says %u",
                stream_info.channels, probe_info.channels),
            stream_info.channels == probe_info.channels);
    ASSERTm(msg("samplerate: the stream says %g, the file says %g",
                stream_info.samplerate, probe_info.samplerate),
            stream_info.samplerate == probe_info.samplerate);
    ASSERTm(msg("duration: the stream says %g, the file says %g",
                stream_info.duration, probe_info.duration),
            stream_info.duration == probe_info.duration);
    ASSERTm(msg("format: the stream says %s, the file says %s",
                stream_info.format, probe_info.format),
            strcmp(stream_info.format, probe_info.format) == 0);

    ASSERTm("info", af_stream_info(s, &stream_info) == AF_OK);
    ASSERTm(msg("info channels %u", stream_info.channels),
            stream_info.channels == probe_info.channels);

    af_stream_free(s);
    PASS();
}

SUITE(reading)
{
    struct { const char *name; uint32_t channels; } cases[] = {
        { "sine_mono_44100_s16.wav",  1 },
        { "sine_mono_44100_f32.wav",  1 },
        { "sine_mono_44100_s16.aiff", 1 },
        { "sine_mono_44100_s24.aiff", 1 },
        { "trailing_chunk.wav",       1 },
        { "sine_mono_44100.flac",     1 }
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        greatest_set_test_suffix(cases[i].name);
        RUN_TESTp(reads_end_to_end, cases[i].name, cases[i].channels);
    }

    RUN_TEST(a_truncated_file_reads_short);
    RUN_TEST(a_stereo_file_keeps_its_channels_apart);
    RUN_TEST(info_describes_the_file_not_the_playback);
}

/* ------------------------------------------------------------------ */
/* Seeking                                                            */
/* ------------------------------------------------------------------ */

TEST a_seek_lands_where_requested(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;
    int        lag = 0;
    double     err;
    uint64_t   target = 7031;   /* not a whole number of cycles */

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(4096, 1);
    af_stream_set_playing(s, 1);

    /* Let the buffer fill with the start of the file, so that a seek that
     * failed to take would be caught handing back frames from around zero. */
    drain(s, buf, 2048, 64, 1, 2000);

    ASSERTm("seek",
            af_stream_seek_seconds(s, (double)target / FIXTURE_RATE) == AF_OK);

    d = drain(s, buf, 512, 64, 1, 2000);
    ASSERTm("timed out after seeking", !d.timed_out);
    ASSERTm(msg("got %lu frames after seeking", (unsigned long)d.frames),
            d.frames == 512);

    err = phase_error(buf + AF_PRIME, 512 - AF_PRIME,
                      (double)(target + AF_PRIME), 1.0, &lag);
    ASSERTm(msg("the block after the seek is off by %.4f (best lag %d): "
                "stale samples, or the wrong position", err, lag),
            err < 0.02);

    ASSERTm(msg("position reads %llu, wanted about %llu",
                (unsigned long long)tell_frames(s),
                (unsigned long long)(target + 512)),
            labs((long)tell_frames(s) - (long)(target + 512)) <= 64);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST repeated_seeks_land_where_requested(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;
    int        i;
    int        bad = 0;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(4096, 1);
    af_stream_set_playing(s, 1);
    drain(s, buf, 2048, 64, 1, 2000);

    for (i = 0; i < 16; i++) {
        uint64_t where = (uint64_t)(1000 + i * 613);
        af_stream_seek_seconds(s, (double)where / FIXTURE_RATE);
        d = drain(s, buf, 256, 64, 1, 2000);
        if (d.frames != 256) { bad++; continue; }
        if (phase_error(buf + AF_PRIME, 256 - AF_PRIME,
                        (double)(where + AF_PRIME), 1.0, NULL) >= 0.02) bad++;
    }
    ASSERTm(msg("%d of 16 repeated seeks came back wrong", bad), bad == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_seek_past_the_end_clamps(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(4096, 1);
    af_stream_set_playing(s, 1);

    ASSERTm("seek past the end",
            af_stream_seek_seconds(s, FIXTURE_FRAMES * 4 / FIXTURE_RATE) == AF_OK);
    ASSERTm("an event was waiting before anything was read",
            !af_stream_pending(s));
    af_stream_read(s, buf, 64);
    ASSERTm("nothing was waiting after reading past the end",
            af_stream_pending(s));
    ASSERTm("the second look found nothing: it was consumed",
            af_stream_pending(s));

    d = drain(s, buf, 512, 64, 1, 1000);
    ASSERTm(msg("eof reported %d times after seeking past the end", d.eofs),
            d.eofs == 1);
    ASSERTm(msg("%lu frames came back from beyond the end",
                (unsigned long)d.frames),
            d.frames == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_seek_with_no_file_open_is_an_error(void)
{
    af_stream *s;
    af_info    info;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);
    af_stream_close(s);
    ASSERTm("seek without a file",
            af_stream_seek_seconds(s, 0.0) == AF_ERR_NOFILE);

    af_stream_free(s);
    PASS();
}

SUITE(seeking)
{
    RUN_TEST(a_seek_lands_where_requested);
    RUN_TEST(repeated_seeks_land_where_requested);
    RUN_TEST(a_seek_past_the_end_clamps);
    RUN_TEST(a_seek_with_no_file_open_is_an_error);
}

/* ------------------------------------------------------------------ */
/* Speed, and the output rate                                         */
/* ------------------------------------------------------------------ */

TEST speed_2_halves_the_output_length(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;
    int        lag = 0;
    double     err;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(FIXTURE_FRAMES, 1);
    af_stream_set_speed(s, 2.0);
    af_stream_set_playing(s, 1);

    d = drain(s, buf, FIXTURE_FRAMES, 64, 1, 5000);
    ASSERTm("timed out", !d.timed_out);
    ASSERTm(msg("produced %lu frames at speed 2, wanted about %d",
                (unsigned long)d.frames, FIXTURE_FRAMES / 2),
            labs((long)d.frames - (long)(FIXTURE_FRAMES / 2)) <= 8);

    /* Each output frame steps two frames through the file. */
    err = phase_error(buf + AF_PRIME, 256, 2.0 * AF_PRIME, 2.0, &lag);
    ASSERTm(msg("the sine at speed 2 is off by %.4f at lag %d", err, lag),
            err < 0.05);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST speed_half_doubles_the_output_length(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(FIXTURE_FRAMES * 3, 1);
    af_stream_set_speed(s, 0.5);
    af_stream_set_playing(s, 1);

    d = drain(s, buf, FIXTURE_FRAMES * 3, 64, 1, 8000);
    ASSERTm("timed out", !d.timed_out);
    ASSERTm(msg("produced %lu frames at speed 0.5, wanted about %d",
                (unsigned long)d.frames, FIXTURE_FRAMES * 2),
            labs((long)d.frames - (long)(FIXTURE_FRAMES * 2)) <= 8);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_file_at_another_rate_is_resampled(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;

    OPEN_FIXTURE(s, info, "sine_mono_48000_s16.wav", 1, 44100.0);
    ASSERTm(msg("the file reports %g Hz", info.samplerate),
            info.samplerate == 48000.0);

    buf = scratch_buffer(FIXTURE_FRAMES * 2, 1);
    af_stream_set_playing(s, 1);
    d = drain(s, buf, FIXTURE_FRAMES * 2, 64, 1, 5000);
    ASSERTm("timed out", !d.timed_out);

    /* 22050 frames of 48000 Hz material is 0.459 s, which is 20256 frames
     * at 44100 Hz. */
    ASSERTm(msg("produced %lu frames, wanted about 20256",
                (unsigned long)d.frames),
            labs((long)d.frames - 20256L) <= 16);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST changing_the_output_rate_keeps_the_file_and_the_position(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;
    uint64_t   before, after;
    int        lag = 0;
    double     err;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(8192, 1);
    af_stream_set_playing(s, 1);
    d = drain(s, buf, 4096, 64, 1, 3000);
    ASSERTm(msg("got %lu frames before the rate changed",
                (unsigned long)d.frames),
            d.frames == 4096);
    before = tell_frames(s);

    af_stream_set_output_samplerate(s, 48000.0);

    ASSERTm("the file is still open", af_stream_info(s, &info) == AF_OK);
    ASSERTm(msg("info still describes the file, at %g", info.samplerate),
            info.samplerate == FIXTURE_RATE);
    ASSERTm(msg("the position moved from %llu to %llu",
                (unsigned long long)before,
                (unsigned long long)tell_frames(s)),
            labs((long)tell_frames(s) - (long)before) <= 64);

    /* The file is reopened and sought behind this, so let it settle before
     * measuring what comes out. */
    drain(s, buf, 512, 64, 1, 3000);
    before = tell_frames(s);

    /* 4800 frames at 48000 Hz is 4410 frames of a 44100 Hz file. */
    d = drain(s, buf, 4800, 64, 1, 3000);
    ASSERTm("timed out after the rate changed", !d.timed_out);
    ASSERTm(msg("got %lu frames at the new rate", (unsigned long)d.frames),
            d.frames == 4800);

    after = tell_frames(s);
    ASSERTm(msg("the position advanced by %ld file frames, wanted about 4410",
                (long)(after - before)),
            labs((long)(after - before) - 4410L) <= 64);

    err = phase_error(buf + AF_PRIME, 512, (double)(before + AF_PRIME),
                      FIXTURE_RATE / 48000.0, &lag);
    ASSERTm(msg("the sine after the rate change is off by %.4f at lag %d",
                err, lag),
            err < 0.05);

    free(buf);
    af_stream_free(s);
    PASS();
}

SUITE(speed)
{
    RUN_TEST(speed_2_halves_the_output_length);
    RUN_TEST(speed_half_doubles_the_output_length);
    RUN_TEST(a_file_at_another_rate_is_resampled);
    RUN_TEST(changing_the_output_rate_keeps_the_file_and_the_position);
}

/* ------------------------------------------------------------------ */
/* Looping                                                            */
/* ------------------------------------------------------------------ */

TEST looping_runs_past_the_end_without_reporting_one(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;
    uint64_t   position;
    int        lag = 0;
    double     err;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer((size_t)FIXTURE_FRAMES * 2 + 4096, 1);
    af_stream_set_looping(s, 1);
    af_stream_set_playing(s, 1);

    d = drain(s, buf, FIXTURE_FRAMES + 4096, 64, 1, 8000);
    ASSERTm("timed out", !d.timed_out);
    ASSERTm(msg("eof reported %d times while looping", d.eofs), d.eofs == 0);
    ASSERTm(msg("produced %lu frames while looping", (unsigned long)d.frames),
            d.frames == (size_t)FIXTURE_FRAMES + 4096);

    position = tell_frames(s);
    ASSERTm(msg("position reads %llu after wrapping a %d frame file",
                (unsigned long long)position, FIXTURE_FRAMES),
            position < (uint64_t)FIXTURE_FRAMES);

    err = phase_error(buf + FIXTURE_FRAMES + AF_PRIME, 512,
                      (double)AF_PRIME, 1.0, &lag);
    ASSERTm(msg("the material after the loop point is off by %.4f", err),
            err < 0.05);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST switching_looping_off_lets_the_file_end(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer((size_t)FIXTURE_FRAMES * 2 + 4096, 1);
    af_stream_set_looping(s, 1);
    af_stream_set_playing(s, 1);

    d = drain(s, buf, FIXTURE_FRAMES + 4096, 64, 1, 8000);
    ASSERTm("timed out while looping", !d.timed_out);
    ASSERTm(msg("position is inside the file: %llu",
                (unsigned long long)tell_frames(s)),
            tell_frames(s) <= (uint64_t)FIXTURE_FRAMES);

    af_stream_set_looping(s, 0);
    d = drain(s, buf, FIXTURE_FRAMES * 2, 64, 1, 8000);
    ASSERTm(msg("eof reported %d times after looping was switched off", d.eofs),
            d.eofs == 1);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST playing_a_spent_file_starts_it_again(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer((size_t)FIXTURE_FRAMES * 2, 1);
    af_stream_set_playing(s, 1);
    d = drain(s, buf, (size_t)FIXTURE_FRAMES * 2, 64, 1, 8000);
    ASSERTm("timed out on the first play", !d.timed_out);
    ASSERTm(msg("eof reported %d times, wanted once", d.eofs), d.eofs == 1);

    /* The transport stopped and started again, on a file with nothing left. */
    af_stream_set_playing(s, 0);
    af_stream_set_playing(s, 1);

    d = drain(s, buf, (size_t)FIXTURE_FRAMES * 2, 64, 1, 8000);
    ASSERTm("timed out on the second play", !d.timed_out);
    ASSERTm(msg("second play produced %lu frames of a %d frame file",
                (unsigned long)d.frames, FIXTURE_FRAMES),
            labs((long)d.frames - (long)FIXTURE_FRAMES) <= 8);
    ASSERTm(msg("eof reported %d times on the second play", d.eofs),
            d.eofs == 1);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_spent_file_holds_its_place_where_that_is_asked_for(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    af_drain   d;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer((size_t)FIXTURE_FRAMES * 2, 1);
    af_stream_set_autorestart(s, 0);
    af_stream_set_playing(s, 1);
    d = drain(s, buf, (size_t)FIXTURE_FRAMES * 2, 64, 1, 8000);
    ASSERTm("timed out on the first play", !d.timed_out);

    af_stream_set_playing(s, 0);
    af_stream_set_playing(s, 1);

    d = drain(s, buf, (size_t)FIXTURE_FRAMES, 64, 1, 2000);
    ASSERTm(msg("second play produced %lu frames with autorestart off",
                (unsigned long)d.frames),
            d.frames == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

SUITE(looping)
{
    RUN_TEST(looping_runs_past_the_end_without_reporting_one);
    RUN_TEST(switching_looping_off_lets_the_file_end);
    RUN_TEST(playing_a_spent_file_starts_it_again);
    RUN_TEST(a_spent_file_holds_its_place_where_that_is_asked_for);
}

/* ------------------------------------------------------------------ */
/* Dropout reporting                                                  */
/* ------------------------------------------------------------------ */

TEST a_cold_start_does_not_report_a_dropout(void)
{
    af_info info;
    float  *buf;
    int     trial;
    int     trials_with_reports = 0;

    buf = scratch_buffer(4096, 1);

    for (trial = 0; trial < 20; trial++) {
        af_stream *s;
        int reports = 0;
        int i;

        OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

        af_stream_set_playing(s, 1);
        for (i = 0; i < 8; i++) {
            af_stream_read(s, buf, 64);
            if (af_stream_read_events(s) & AF_EVENT_UNDERFLOW) reports++;
        }
        if (reports > 0) trials_with_reports++;

        af_stream_free(s);
    }

    ASSERTm(msg("%d of 20 cold starts reported underflow before delivering "
                "a frame", trials_with_reports),
            trials_with_reports == 0);

    free(buf);
    PASS();
}

TEST a_seek_into_an_unbuffered_region_does_not_report_a_dropout(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    int        trial;
    int        trials_with_reports = 0;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(4096, 1);
    af_stream_set_playing(s, 1);
    drain(s, buf, 1024, 64, 1, 2000);
    af_stream_read_events(s);

    for (trial = 0; trial < 20; trial++) {
        int reports = 0;
        int i;

        ASSERTm("seek",
                af_stream_seek_seconds(s, (500 + trial * 311) / FIXTURE_RATE) == AF_OK);
        for (i = 0; i < 8; i++) {
            af_stream_read(s, buf, 64);
            if (af_stream_read_events(s) & AF_EVENT_UNDERFLOW) reports++;
        }
        if (reports > 0) trials_with_reports++;
    }

    ASSERTm(msg("%d of 20 seeks reported underflow while re-buffering",
                trials_with_reports),
            trials_with_reports == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_stall_is_reported_once_not_once_per_block(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    int        short_reads = 0, reports = 0;
    int        i;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(8192, 1);
    af_stream_set_looping(s, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));

    af_stream_set_speed(s, AF_SPEED_MAX);
    for (i = 0; i < 40; i++) {
        if (af_stream_read(s, buf, 8192) < 8192) short_reads++;
        if (af_stream_read_events(s) & AF_EVENT_UNDERFLOW) reports++;
    }
    ASSERTm(msg("%d of 40 reads fell short: nothing stalled", short_reads),
            short_reads == 40);
    ASSERTm(msg("%d underflow reports across one stall", reports),
            reports == 1);

    ASSERTm("seek", af_stream_seek_seconds(s, 0.0) == AF_OK);
    af_stream_set_speed(s, 1.0);
    ASSERTm("the stream never got going again", settle(s, buf, 64));

    af_stream_set_speed(s, AF_SPEED_MAX);
    short_reads = reports = 0;
    for (i = 0; i < 40; i++) {
        if (af_stream_read(s, buf, 8192) < 8192) short_reads++;
        if (af_stream_read_events(s) & AF_EVENT_UNDERFLOW) reports++;
    }
    ASSERTm(msg("%d of 40 reads fell short in the second stall", short_reads),
            short_reads == 40);
    ASSERTm(msg("%d underflow reports across the second stall", reports),
            reports == 1);

    free(buf);
    af_stream_free(s);
    PASS();
}

SUITE(dropouts)
{
    RUN_TEST(a_cold_start_does_not_report_a_dropout);
    RUN_TEST(a_seek_into_an_unbuffered_region_does_not_report_a_dropout);
    RUN_TEST(a_stall_is_reported_once_not_once_per_block);
}

/* ------------------------------------------------------------------ */
/* Cueing                                                             */
/* ------------------------------------------------------------------ */

#define CUE_PREROLL_FRAMES 1984

/* Reads a block a millisecond, slower than a 64-frame block plays, until
 * AF_EVENT_CUED, and keeps what the current place played into `played`, which
 * holds `capacity` frames. Counts the blocks that came back short. Returns
 * the frames kept, or -1 if the cue never came. */
static long wait_for_cue(af_stream *s, float *played, size_t capacity,
                         size_t block, int *short_blocks)
{
    size_t kept = 0;
    int    waited_ms = 0;
    float  scratch[256];

    *short_blocks = 0;
    for (;;) {
        float *to = (played != NULL && kept + block <= capacity) ? played + kept : scratch;
        size_t got = af_stream_read(s, to, block);

        if (to != scratch) kept += got;
        if (got < block) (*short_blocks)++;
        if (af_stream_read_events(s) & AF_EVENT_CUED) return (long)kept;
        af_sleep_ms(1);
        if (++waited_ms > 2000) return -1;
    }
}

static double frames_to_seconds(double frames) { return frames / FIXTURE_RATE; }

/* The largest difference from the fixture's sine when `buf` starts at file
 * frame `start` and each frame advances `step` file frames. The 48000 Hz
 * fixture has the same 100-frame cycle in its own frames. */
static double offset_error(const float *buf, size_t frames, double start,
                           double step)
{
    double worst = 0.0;
    size_t i;

    for (i = 0; i < frames; i++) {
        double err = fabs((double)buf[i] - expected_sample(start + (double)i * step));
        if (!isfinite(err)) return INFINITY;
        if (err > worst) worst = err;
    }
    return worst;
}

/* The fractional offset in file frames, within three either way, at which
 * `buf` best fits the fixture from `start`, and the error there. */
static double fit_offset(const float *buf, size_t frames, double start,
                         double step, double *err)
{
    double best = 1e9, best_offset = 0.0, offset;

    for (offset = -3.0; offset <= 3.0; offset += 0.01) {
        double e = offset_error(buf, frames, start + offset, step);
        if (e < best) { best = e; best_offset = offset; }
    }
    *err = best;
    return best_offset;
}

TEST a_cue_hands_over_the_preroll_and_continues_from_the_cue_point(void)
{
    af_stream *s;
    af_info    info;
    float     *played, *joined;
    double     t0 = 0.0, cued = 0.0, err;
    int        lag = 99, short_blocks = 0;
    long       kept;
    size_t     frames;
    uint64_t   target = 9031;   /* not a whole number of cycles */

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    played = scratch_buffer(FIXTURE_FRAMES, 1);
    joined = scratch_buffer(CUE_PREROLL_FRAMES + 512, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, played, 64));
    drain(s, played, 2048 - 64, 64, 1, 2000);

    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds((double)target),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));

    kept = wait_for_cue(s, played, FIXTURE_FRAMES, 64, &short_blocks);
    ASSERTm("the cue never became ready", kept >= 0);
    ASSERTm(msg("%d blocks came back short while the cue was prepared",
                short_blocks), short_blocks == 0);
    /* The stream has played from the file's start, so the resampler's frame of
     * history puts it one frame behind. */
    if (kept > 0) {
        err = phase_error(played, (size_t)kept, 2047.0, 1.0, &lag);
        ASSERTm(msg("the current place did not keep playing: off by %.4f at "
                    "lag %d", err, lag), err < 0.02 && lag == 0);
    }

    frames = af_stream_cue_frames(s);
    ASSERT_EQ_FMT((size_t)CUE_PREROLL_FRAMES, frames, "%lu");
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, joined, &t0, &cued));
    ASSERT_IN_RANGE(frames_to_seconds((double)(target - CUE_PREROLL_FRAMES)), t0, 1e-9);
    ASSERT_IN_RANGE(frames_to_seconds((double)target), cued, 1e-9);

    ASSERT_EQ_FMT((size_t)512, af_stream_read(s, joined + frames, 512), "%lu");

    /* The pre-roll from its first frame, the join and the stream after it,
     * as one run of the file. */
    err = phase_error(joined, frames + 512, (double)(target - frames), 1.0, &lag);
    ASSERTm(msg("pre-roll and stream are off by %.6f at lag %d", err, lag),
            err < 1e-4 && lag == 0);

    ASSERT_EQ(AF_ERR_ARGS, af_stream_take_cue(s, joined, &t0, &cued));

    free(joined);
    free(played);
    af_stream_free(s);
    PASS();
}

TEST a_cue_reuses_a_voice_without_its_old_place(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued, err;
    int        i, lag = 99, short_blocks;
    uint64_t   targets[3] = { 15000, 4321, 12777 };

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(CUE_PREROLL_FRAMES + 256, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));

    /* The third cue goes to the voice the first one played on. */
    for (i = 0; i < 3; i++) {
        ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds((double)targets[i]),
                                       frames_to_seconds(CUE_PREROLL_FRAMES)));
        ASSERTm("the cue never became ready",
                wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
        ASSERT_EQ(AF_OK, af_stream_take_cue(s, buf, &t0, &cued));
        ASSERT_EQ_FMT((size_t)256, af_stream_read(s, buf + CUE_PREROLL_FRAMES, 256), "%lu");

        err = phase_error(buf, CUE_PREROLL_FRAMES + 256,
                          (double)(targets[i] - CUE_PREROLL_FRAMES), 1.0, &lag);
        ASSERTm(msg("cue %d is off by %.6f at lag %d", i + 1, err, lag),
                err < 1e-4 && lag == 0);

        drain(s, buf, 1500, 64, 1, 2000);
    }

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_cue_nearer_the_start_than_its_preroll_is_padded_with_silence(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued, err;
    int        short_blocks, lag = 99;
    size_t     i, zeros = CUE_PREROLL_FRAMES - 500;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(8000, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));
    drain(s, buf, 8000, 64, 1, 2000);

    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(500.0),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));
    ASSERTm("the cue never became ready",
            wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, buf, &t0, &cued));
    ASSERT_IN_RANGE(frames_to_seconds(500.0 - CUE_PREROLL_FRAMES), t0, 1e-9);
    ASSERT_EQ_FMT((size_t)256, af_stream_read(s, buf + CUE_PREROLL_FRAMES, 256), "%lu");

    for (i = 0; i < zeros; i++) {
        if (buf[i] != 0.0f) FAILm(msg("frame %lu before the file's start is %g",
                                      (unsigned long)i, buf[i]));
    }
    err = phase_error(buf + zeros, 500 + 256, 0.0, 1.0, &lag);
    ASSERTm(msg("the file's start is off by %.6f at lag %d", err, lag),
            err < 1e-4 && lag == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_cue_near_the_end_plays_to_the_end_and_no_further(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued;
    int        short_blocks;
    af_drain   d;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(FIXTURE_FRAMES, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));

    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(FIXTURE_FRAMES - 1000.0),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));
    ASSERTm("the cue never became ready",
            wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, buf, &t0, &cued));

    d = drain(s, buf, FIXTURE_FRAMES, 64, 1, 2000);
    ASSERTm("timed out", !d.timed_out);
    ASSERT_EQ_FMT(1, d.eofs, "%d");
    ASSERTm(msg("played %lu frames after the cue, wanted 1000",
                (unsigned long)d.frames), labs((long)d.frames - 1000L) <= 1);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_second_cue_replaces_the_first(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued, err;
    int        short_blocks, lag = 99;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(CUE_PREROLL_FRAMES + 256, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));

    /* Back to back, so the first is still seeking when the second comes. */
    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(16000.0),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));
    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(6007.0),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));
    ASSERTm("the cue never became ready",
            wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, buf, &t0, &cued));
    ASSERT_IN_RANGE(frames_to_seconds(6007.0), cued, 1e-9);
    ASSERT_EQ_FMT((size_t)256, af_stream_read(s, buf + CUE_PREROLL_FRAMES, 256), "%lu");

    err = phase_error(buf, CUE_PREROLL_FRAMES + 256, 6007.0 - CUE_PREROLL_FRAMES,
                      1.0, &lag);
    ASSERTm(msg("off by %.6f at lag %d: the first cue's place", err, lag),
            err < 1e-4 && lag == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_seek_cancels_a_cue(void)
{
    af_stream *s;
    af_info    info;
    float      buf[64];
    double     t0, cued;
    int        i, cued_events = 0;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));

    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(9031.0),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));
    ASSERT_EQ(AF_OK, af_stream_seek_seconds(s, frames_to_seconds(3000.0)));

    for (i = 0; i < 200; i++) {
        af_stream_read(s, buf, 64);
        if (af_stream_read_events(s) & AF_EVENT_CUED) cued_events++;
        af_sleep_ms(1);
    }
    ASSERT_EQ_FMT(0, cued_events, "%d");
    ASSERT_EQ_FMT((size_t)0, af_stream_cue_frames(s), "%lu");
    ASSERT_EQ(AF_ERR_ARGS, af_stream_take_cue(s, buf, &t0, &cued));

    af_stream_free(s);
    PASS();
}

TEST a_cue_made_while_stopped_lands_and_playing_starts_there(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued, err;
    int        short_blocks, lag = 99;
    af_drain   d;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(CUE_PREROLL_FRAMES + 512, 1);

    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(9031.0),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));
    ASSERTm("the cue never became ready",
            wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, buf, &t0, &cued));
    ASSERT_EQ_FMT((size_t)0, af_stream_read(s, buf + CUE_PREROLL_FRAMES, 64), "%lu");

    af_stream_set_playing(s, 1);
    d = drain(s, buf + CUE_PREROLL_FRAMES, 512, 64, 1, 2000);
    ASSERTm("timed out", !d.timed_out);
    err = phase_error(buf, CUE_PREROLL_FRAMES + 512, 9031.0 - CUE_PREROLL_FRAMES,
                      1.0, &lag);
    ASSERTm(msg("off by %.6f at lag %d", err, lag), err < 1e-4 && lag == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_cue_into_a_resampled_file_joins_the_preroll_to_the_stream(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued, err, offset, step = 48000.0 / 44100.0;
    int        short_blocks;
    size_t     frames;

    OPEN_FIXTURE(s, info, "sine_mono_48000_s16.wav", 1, 44100.0);

    buf = scratch_buffer(CUE_PREROLL_FRAMES + 512, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));

    ASSERT_EQ(AF_OK, af_stream_cue(s, 0.25, frames_to_seconds(CUE_PREROLL_FRAMES)));
    ASSERTm("the cue never became ready",
            wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
    frames = af_stream_cue_frames(s);
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, buf, &t0, &cued));
    ASSERT_EQ_FMT((size_t)512, af_stream_read(s, buf + frames, 512), "%lu");

    /* The decoder's resampler restarts at a seek, with a fractional latency
     * of its own that every seek has. What the cue adds is that the pre-roll
     * and the stream are one run: the offset that fits the pre-roll alone
     * has to fit across the join. */
    offset = fit_offset(buf, frames, t0 * 48000.0, step, &err);
    ASSERTm(msg("the pre-roll is %.2f file frames from t0", offset),
            fabs(offset) <= 2.0);
    err = offset_error(buf, frames + 512, t0 * 48000.0 + offset, step);
    ASSERTm(msg("off by %.5f across the join at the pre-roll's offset %.2f",
                err, offset), err < 1e-3);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_cue_refuses_what_it_cannot_do(void)
{
    af_stream *s;
    af_info    info;

    ASSERT_EQ(AF_OK, af_stream_new(&s, 1, FIXTURE_RATE));
    ASSERT_EQ(AF_ERR_NOFILE, af_stream_cue(s, 0.1, 0.01));
    ASSERT_EQ(AF_OK, af_stream_open(s, fixture("sine_mono_44100_f32.wav"), &info));
    ASSERT_EQ(AF_ERR_ARGS, af_stream_cue(s, 0.1, -0.01));
    ASSERT_EQ(AF_ERR_ARGS, af_stream_cue(s, 0.1, AF_CUE_PREROLL_MAX + 0.01));
    ASSERT_EQ(AF_OK, af_stream_cue(s, 0.1, AF_CUE_PREROLL_MAX));

    /* At speed 2 the output is short and the file is the limit; at half
     * speed the reverse. */
    af_stream_set_speed(s, 2.0);
    ASSERT_EQ(AF_ERR_ARGS, af_stream_cue(s, 0.1, AF_CUE_PREROLL_MAX + 0.01));
    af_stream_set_speed(s, 0.5);
    ASSERT_EQ(AF_ERR_ARGS, af_stream_cue(s, 0.1, AF_CUE_PREROLL_MAX / 2.0 + 0.01));
    ASSERT_EQ(AF_OK, af_stream_cue(s, 0.1, AF_CUE_PREROLL_MAX / 2.0));
    af_stream_close(s);
    ASSERT_EQ_FMT((size_t)0, af_stream_cue_frames(s), "%lu");
    af_stream_free(s);
    PASS();
}

/* Copies the named fixture to `to`. */
static int copy_fixture(const char *name, const char *to)
{
    FILE  *in = fopen(fixture(name), "rb"), *out;
    char   chunk[8192];
    size_t n;

    if (in == NULL) return 0;
    out = fopen(to, "wb");
    if (out == NULL) { fclose(in); return 0; }
    while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) fwrite(chunk, 1, n, out);
    fclose(in);
    return fclose(out) == 0;
}

TEST a_cue_reads_the_file_open_already(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued, err;
    int        short_blocks, lag = 99;
    char       path[1024];

    snprintf(path, sizeof(path), "%s/cue_while_gone_%ld.wav", g_dir, af_process_id());
    ASSERTm("copy", copy_fixture("sine_mono_44100_f32.wav", path));

    ASSERT_EQ(AF_OK, af_stream_new(&s, 1, FIXTURE_RATE));
    ASSERT_EQ(AF_OK, af_stream_open(s, path, &info));

    /* Where an open file cannot be removed, there is nothing to show. */
    if (remove(path) != 0) {
        af_stream_free(s);
        remove(path);
        SKIPm("an open file cannot be removed here");
    }

    buf = scratch_buffer(CUE_PREROLL_FRAMES + 256, 1);
    af_stream_set_playing(s, 1);
    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(9031.0),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));
    ASSERTm("the cue never became ready",
            wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, buf, &t0, &cued));
    ASSERT_EQ_FMT((size_t)256, af_stream_read(s, buf + CUE_PREROLL_FRAMES, 256), "%lu");

    err = phase_error(buf, CUE_PREROLL_FRAMES + 256, 9031.0 - CUE_PREROLL_FRAMES,
                      1.0, &lag);
    ASSERTm(msg("off by %.6f at lag %d", err, lag), err < 1e-4 && lag == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_cue_made_while_a_speed_change_waits_keeps_its_own_speed(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued, err;
    int        short_blocks, lag = 99;
    size_t     frames;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(CUE_PREROLL_FRAMES + 512, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));

    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(9031.0),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));
    af_stream_set_speed(s, 2.0);
    ASSERTm("the cue never became ready",
            wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
    frames = af_stream_cue_frames(s);
    ASSERT_EQ_FMT((size_t)CUE_PREROLL_FRAMES, frames, "%lu");
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, buf, &t0, &cued));
    ASSERT_EQ_FMT((size_t)256, af_stream_read(s, buf + frames, 256), "%lu");

    err = phase_error(buf, frames, 9031.0 - CUE_PREROLL_FRAMES, 1.0, &lag);
    ASSERTm(msg("the pre-roll is off by %.6f at lag %d at the speed of the cue",
                err, lag), err < 1e-4 && lag == 0);

    /* The resampler takes a few frames to move to the new ratio. */
    err = phase_error(buf + frames + AF_PRIME, 256 - AF_PRIME,
                      9031.0 + 2.0 * AF_PRIME, 2.0, &lag);
    ASSERTm(msg("the stream is off by %.4f at lag %d at the new speed", err, lag),
            err < 0.02);

    free(buf);
    af_stream_free(s);
    PASS();
}

/* At speed 2 a cue to the first frame with no pre-roll has nothing before it
 * to read. The first cue leaves the other voice somewhere else, so a seek
 * that never happened would show. */
TEST a_cue_with_nothing_to_read_still_lands(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued, err;
    int        short_blocks, lag = 99;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(512, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));
    drain(s, buf, 512, 64, 1, 2000);
    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(15000.0), 0.0));
    ASSERTm("the first cue never became ready",
            wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, NULL, &t0, &cued));
    drain(s, buf, 512, 64, 1, 2000);

    af_stream_set_speed(s, 2.0);
    ASSERT_EQ(AF_OK, af_stream_cue(s, 0.0, 0.0));
    ASSERTm("the cue never became ready",
            wait_for_cue(s, NULL, 0, 64, &short_blocks) >= 0);
    ASSERT_EQ(AF_OK, af_stream_take_cue(s, NULL, &t0, &cued));
    ASSERT_EQ_FMT((size_t)256, af_stream_read(s, buf, 256), "%lu");

    /* The first frame after the resampler's history is the file's second. */
    err = phase_error(buf, 256, 1.0, 2.0, &lag);
    ASSERTm(msg("off by %.6f at lag %d: not from the start of the file", err, lag),
            err < 1e-4 && lag == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST the_place_left_ending_before_a_cue_lands_is_not_an_end(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    double     t0, cued;
    int        eofs = 0, i;
    size_t     got = 0;

    OPEN_FIXTURE(s, info, "sine_mono_44100_f32.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(FIXTURE_FRAMES, 1);
    af_stream_set_playing(s, 1);
    ASSERTm("the stream never got going", settle(s, buf, 64));
    drain(s, buf, FIXTURE_FRAMES - 64 - 64, 64, 1, 2000);

    /* 64 frames are left: the place runs out a block or two into the cue. */
    ASSERT_EQ(AF_OK, af_stream_cue(s, frames_to_seconds(9031.0),
                                   frames_to_seconds(CUE_PREROLL_FRAMES)));
    for (i = 0; i < 2000; i++) {
        unsigned events;

        af_stream_read(s, buf, 64);
        events = af_stream_read_events(s);
        if (events & AF_EVENT_EOF) eofs++;
        if (events & AF_EVENT_CUED) break;
        af_sleep_ms(1);
    }
    ASSERTm("the cue never became ready", i < 2000);
    ASSERT_EQ_FMT(0, eofs, "%d");

    ASSERT_EQ(AF_OK, af_stream_take_cue(s, NULL, &t0, &cued));
    for (i = 0; i < 8; i++) got += af_stream_read(s, buf, 64);
    ASSERT_EQ_FMT((size_t)512, got, "%lu");
    ASSERTm("no end reported once the cue has taken effect",
            !(af_stream_read_events(s) & AF_EVENT_EOF));

    free(buf);
    af_stream_free(s);
    PASS();
}

SUITE(cueing)
{
    RUN_TEST(a_cue_made_while_a_speed_change_waits_keeps_its_own_speed);
    RUN_TEST(a_cue_with_nothing_to_read_still_lands);
    RUN_TEST(the_place_left_ending_before_a_cue_lands_is_not_an_end);
    RUN_TEST(a_cue_reads_the_file_open_already);
    RUN_TEST(a_cue_hands_over_the_preroll_and_continues_from_the_cue_point);
    RUN_TEST(a_cue_reuses_a_voice_without_its_old_place);
    RUN_TEST(a_cue_nearer_the_start_than_its_preroll_is_padded_with_silence);
    RUN_TEST(a_cue_near_the_end_plays_to_the_end_and_no_further);
    RUN_TEST(a_second_cue_replaces_the_first);
    RUN_TEST(a_seek_cancels_a_cue);
    RUN_TEST(a_cue_made_while_stopped_lands_and_playing_starts_there);
    RUN_TEST(a_cue_into_a_resampled_file_joins_the_preroll_to_the_stream);
    RUN_TEST(a_cue_refuses_what_it_cannot_do);
}

/* ------------------------------------------------------------------ */
/* Opening, closing and freeing                                       */
/* ------------------------------------------------------------------ */

TEST opening_a_second_file_replaces_the_first(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(4096, 1);
    af_stream_set_playing(s, 1);
    drain(s, buf, 2048, 64, 1, 2000);

    ASSERTm("second open",
            af_stream_open(s, fixture("sine_mono_48000_s16.wav"), &info) == AF_OK);
    ASSERTm("info follows the new file", info.samplerate == 48000.0);
    ASSERTm("playback did not stop", af_stream_read(s, buf, 64) == 0);
    ASSERTm("the position did not go back to zero",
            af_stream_tell_seconds(s) == 0.0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST a_failed_open_leaves_the_file_that_was_open_alone(void)
{
    af_stream *s;
    af_info    info;
    af_status  status;

    OPEN_FIXTURE(s, info, "sine_mono_48000_s16.wav", 1, FIXTURE_RATE);

    status = af_stream_open(s, fixture("no_such_file.wav"), &info);
    ASSERTm(msg("status %s", af_status_string(status)), status == AF_ERR_OPEN);
    ASSERTm("the previous file is still open",
            af_stream_info(s, &info) == AF_OK);
    ASSERTm("and it is still the same one", info.samplerate == 48000.0);

    af_stream_free(s);
    PASS();
}

TEST close_leaves_no_file_to_read(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;

    OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);

    buf = scratch_buffer(4096, 1);
    af_stream_close(s);
    ASSERTm("info after close", af_stream_info(s, &info) == AF_ERR_NOFILE);
    af_stream_set_playing(s, 1);
    ASSERTm("reading after close produced frames",
            af_stream_read(s, buf, 64) == 0);

    free(buf);
    af_stream_free(s);
    PASS();
}

TEST closing_and_freeing_while_the_stream_is_reading_ahead(void)
{
    af_stream *s;
    af_info    info;
    float     *buf;
    int        i;

    buf = scratch_buffer(4096, 1);

    for (i = 0; i < 8; i++) {
        OPEN_FIXTURE(s, info, "sine_mono_44100_s16.wav", 1, FIXTURE_RATE);
        af_stream_set_looping(s, 1);
        af_stream_set_playing(s, 1);
        af_stream_read(s, buf, 64);

        if (i % 2) af_stream_close(s);
        af_stream_free(s);
    }

    free(buf);
    PASS();
}

TEST construction_rejects_what_it_cannot_do(void)
{
    af_stream *s = NULL;

    ASSERTm("zero channels", af_stream_new(&s, 0, FIXTURE_RATE) == AF_ERR_ARGS);
    ASSERTm("no out pointer", af_stream_new(NULL, 2, FIXTURE_RATE) == AF_ERR_ARGS);

    ASSERTm("af_stream_new", af_stream_new(&s, 2, FIXTURE_RATE) == AF_OK);
    ASSERTm("opening a malformed file",
            af_stream_open(s, fixture("garbage.wav"), NULL) == AF_ERR_FORMAT);
    ASSERTm("opening nothing", af_stream_open(s, NULL, NULL) == AF_ERR_ARGS);
    af_stream_free(s);

    /* Freeing a stream that never opened anything has to join cleanly too. */
    ASSERTm("af_stream_new", af_stream_new(&s, 1, FIXTURE_RATE) == AF_OK);
    af_stream_free(s);
    af_stream_free(NULL);
    PASS();
}

SUITE(lifecycle)
{
    RUN_TEST(opening_a_second_file_replaces_the_first);
    RUN_TEST(a_failed_open_leaves_the_file_that_was_open_alone);
    RUN_TEST(close_leaves_no_file_to_read);
    RUN_TEST(closing_and_freeing_while_the_stream_is_reading_ahead);
    RUN_TEST(construction_rejects_what_it_cannot_do);
}

/* ------------------------------------------------------------------ */

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] != '-') g_dir = argv[1];

    /* No fixtures means no Python at configure time, and every case would
     * fail */
    if (!exists(fixture("sine_mono_44100_s16.wav"))) {
        printf("no fixtures in %s: skipping\n", g_dir);
        return 77;
    }

    printf("pd-audiofile core tests, fixtures in %s\n\n", g_dir);

    GREATEST_MAIN_BEGIN();

    RUN_SUITE(probing);
    RUN_SUITE(reading);
    RUN_SUITE(seeking);
    RUN_SUITE(speed);
    RUN_SUITE(looping);
    RUN_SUITE(dropouts);
    RUN_SUITE(cueing);
    RUN_SUITE(lifecycle);

    GREATEST_MAIN_END();
}
