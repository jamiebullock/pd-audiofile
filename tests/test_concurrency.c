/* test_concurrency.c -- control thread and audio thread at once, which
 * test_core never does: it drives both from one thread, so the window in which
 * a file is replaced while a block is being read cannot open.
 *
 * Run under ThreadSanitizer to make this mean anything:
 *   cc -fsanitize=thread ... tests/test_concurrency.c
 *
 * Part of pd-audiofile. SPDX-License-Identifier: Zlib
 */

#include "audiofile_core.h"
#include "af_platform.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTROL_ITERATIONS 4000
#define BLOCK_FRAMES       64
#define CHANNELS           1

static _Atomic bool  g_stop;
static _Atomic long  g_blocks;
static _Atomic long  g_frames;

static void audio_thread(void *data)
{
    af_stream *s = (af_stream *)data;
    float buf[BLOCK_FRAMES * CHANNELS];

    while (!atomic_load(&g_stop)) {
        atomic_fetch_add(&g_frames, (long)af_stream_read(s, buf, BLOCK_FRAMES));
        af_stream_take_events(s);
        atomic_fetch_add(&g_blocks, 1);
    }
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "fixtures";
    char        path[1024];
    af_stream  *s = NULL;
    af_thread   audio;
    long        blocks, frames;
    int         i;

    snprintf(path, sizeof(path), "%s/sine_mono_44100_s16.wav", dir);

    if (af_stream_new(&s, CHANNELS, 44100.0) != AF_OK) {
        printf("af_stream_new failed\n");
        return 2;
    }
    if (af_stream_open(s, path, NULL) != AF_OK) {
        printf("could not open %s\n", path);
        af_stream_free(s);
        return 77;
    }
    af_stream_set_running(s, 1);

    if (!af_thread_start(&audio, audio_thread, s)) {
        printf("could not start the audio thread\n");
        af_stream_free(s);
        return 2;
    }

    for (i = 0; i < CONTROL_ITERATIONS; i++) {
        af_stream_seek_frames(s, (uint64_t)((i * 37) % 20000));
        af_stream_set_speed(s, 1.0 + (i % 5) * 0.1);
        af_stream_set_loop(s, i & 1);
        af_stream_tell_frames(s);
        af_stream_tell_seconds(s);

        if (i % 97 == 0) {
            af_stream_close(s);
            af_stream_open(s, path, NULL);
            af_stream_set_running(s, 1);
        }
        if (i % 211 == 0) {
            af_info info;
            af_stream_info(s, &info);
        }
        /* The heaviest of the teardowns: it rebuilds the engine underneath the
         * audio thread and reopens the file. */
        if (i % 307 == 0) {
            af_stream_set_output_samplerate(s, (i % 614 == 0) ? 48000.0 : 44100.0);
            af_stream_open(s, path, NULL);
            af_stream_set_running(s, 1);
        }
    }

    atomic_store(&g_stop, true);
    af_thread_join(&audio);

    blocks = atomic_load(&g_blocks);
    frames = atomic_load(&g_frames);
    af_stream_free(s);

    if (blocks <= 0) {
        printf("FAIL  the audio thread produced no blocks\n");
        return 1;
    }

    /* Blocks alone would pass on a core that answered every one of them with
     * nothing. */
    if (frames < BLOCK_FRAMES * 100) {
        printf("FAIL  the audio thread was handed %ld frames across %ld blocks\n",
               frames, blocks);
        return 1;
    }

    printf("ok    %d control operations against %ld audio blocks, %ld frames\n",
           CONTROL_ITERATIONS, blocks, frames);
    return 0;
}
