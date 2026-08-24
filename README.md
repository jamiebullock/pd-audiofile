# pd-audiofile

A sound-file library for [Pure Data](https://puredata.info), built on
[miniaudio](https://github.com/mackron/miniaudio).

| Object | Kind | Does |
| --- | --- | --- |
| `[af.info]` | control | reports sample rate, frame count, channels and sample format from a file's header |
| `[af.play~]` | signal | streams a file from disk: start/stop, looping, speed, seek, position |

Formats: WAV, AIFF/AIFC, RF64, W64, FLAC, MP3, and Vorbis where miniaudio's
`extras/stb_vorbis.c` is present at build time.

## Why

`[readsf~]` streams from disk, but has no speed control or resampling, cannot
seek while running, does not loop, does not report its position, and reads only
the formats Pd itself parses. `[soundfiler]` reads a whole file into an array.
Nothing in vanilla Pd says what is in a file without loading it.

The `af` prefix follows macOS's `afinfo` and `afplay`.

## Using it

Both classes are in one binary, so load the library rather than the objects:

```pd
[declare -lib audiofile]
```

or start Pd with `-lib audiofile`.

Paths resolve the way Pd resolves any file: beside the patch first, then along
the search path. Absolute paths work.

### `[af.info]`

No creation arguments. One message outlet.

| Message in | Argument | Meaning |
| --- | --- | --- |
| `open <path>` | symbol | read the header and report |

| Selector out | Arguments | When |
| --- | --- | --- |
| `info` | samplerate, frames, channels, format-symbol | on `open` |
| `error` | symbol | the file could not be read, with a short reason |

`[openpanel]` goes through `[open $1(`, as it does into `[readsf~]`.

### `[af.play~]`

```pd
[af.play~ <channels>]
```

The creation argument is the number of signal outlets, 2 by default. Outlets:
one signal per channel, then a message outlet. A file with a different channel
count is mixed to the object's.

| Message in | Argument | Meaning |
| --- | --- | --- |
| `open <path>` | symbol | open the file and report `info`; does not start playback |
| `info` | | report `info` for the file already open |
| `play <0/1>` | float | start and stop |
| `loop <0/1>` | float | return to the start on reaching the end |
| `speed <f>` | float | playback rate; 1 is the file's own rate |
| `pos <f>` | float | seek, in seconds from the start |
| `getpos` | | report `pos` once |
| `close` | | release the file |

| Selector out | Arguments | When |
| --- | --- | --- |
| `info` | samplerate, frames, channels, format-symbol | after `open`, and on `info` |
| `pos` | seconds | on `getpos` |
| `eof` | | on reaching the end with looping off |
| `error` | symbol | any failure, with a short reason |

`[openpanel]` goes through `[open $1(` here too, and a toggle through
`[play $1(`. `info` reports the same four elements `[af.info]` does.

Rules:

- `open` replaces the file being played and stops playback. A failed `open`
  leaves the file that was open alone.
- A seek past the end clamps there, and with looping off reads as end of file.
- A file at another sample rate is resampled to Pd's, and `speed` multiplies
  that ratio. At speed 2 a file plays twice as fast and an octave higher.
- Negative speed and reverse playback are not supported.
- A seek takes a block or two: nothing comes out in between rather than
  anything stale, and `getpos` answers the target from the moment it is asked.
- Running dry outputs silence and reports `error underflow` once per stall
  rather than once per block. Filling up is not running dry: a fresh `open`, or
  a seek into a region not yet buffered, reports nothing, so `open ..., play 1`
  in one message is fine.

## Building

C99 throughout, so any toolchain from the last fifteen years will do. Nothing
else is needed beyond the dependencies below.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The build fetches what it cannot find, at pinned versions: Pure Data 0.55-2 for
`m_pd.h`, miniaudio 0.11.25, and greatest 1.5.0, which the tests are written
against. Point it at copies you already have to skip that:

```sh
cmake -S . -B build \
  -DPD_INCLUDE_DIR=/Applications/Pd-0.55-2.app/Contents/Resources/src \
  -DMINIAUDIO_INCLUDE_DIR=/path/to/miniaudio
```

On Windows the external has to link against Pd's import library, which source
alone does not provide: add `-DPD_LIBRARY=<path to pd.lib>`.

| Option | Default | Meaning |
| --- | --- | --- |
| `PD_INCLUDE_DIR` | fetched | directory containing `m_pd.h` |
| `PD_LIBRARY` | searched | `pd.lib`; Windows only |
| `MINIAUDIO_INCLUDE_DIR` | fetched | directory containing `miniaudio.h` |
| `GREATEST_INCLUDE_DIR` | fetched | directory containing `greatest.h`; tests only |
| `AUDIOFILE_USE_HOST_MINIAUDIO` | `OFF` | link against the miniaudio the host compiles |
| `AUDIOFILE_HOST_MINIAUDIO_DEFINES` | empty | the `MA_NO_*` macros the host compiled it with |
| `PD_EXECUTABLE` | searched | `pd`, for the smoke test |
| `AUDIOFILE_BUILD_TESTS` | `ON` | build the test binaries |

### Installing

```sh
cmake --install build --prefix ~/Documents/Pd/externals
```

That writes an `audiofile/` directory holding the external, the help patches and
the meta patch: the layout Pd expects on its search path, and the one
[deken](https://github.com/pure-data/deken) packages.

## Embedding: sharing miniaudio with a host application

miniaudio's implementation is compiled in exactly one translation unit, so an
external loaded into an application that already compiles miniaudio must not
compile a second copy. `AUDIOFILE_USE_HOST_MINIAUDIO` decides:

- **`OFF`**, the default: `src/miniaudio_impl.c` is compiled, with
  `MA_NO_DEVICE_IO`, `MA_NO_GENERATION` and `MA_NO_ENCODING`. That keeps the
  decoders, the converter, the resampler and the playback engine, and drops the
  audio backends, which Pd owns.
- **`ON`**: only declarations are included. The host must have compiled
  miniaudio with decoding, the engine, the node graph and the resource manager
  all enabled; each of those missing is a link error.

The host's remaining `MA_NO_*` macros have to be passed in through
`AUDIOFILE_HOST_MINIAUDIO_DEFINES`. Several change the layout of `ma_engine_config`
and `ma_engine`, `MA_NO_DEVICE_IO` alone by five members, so a translation unit
that disagrees writes fields at the wrong offsets. That is not a link error, so
`af_stream_new` checks at run time and answers `config` instead.

```sh
cmake -S . -B build -DAUDIOFILE_USE_HOST_MINIAUDIO=ON \
  -DAUDIOFILE_HOST_MINIAUDIO_DEFINES="MA_NO_DEVICE_IO;MA_NO_GENERATION;MA_NO_ENCODING"
```

## Testing

`tests/test_core.c` covers the core with no Pd involved, in seven suites run by
greatest: the header facts of each fixture, a file read end to end against its
frame count, the phase of the block after a seek, output length against speed
and sample-rate conversion, a forced stall reported once, and the error paths
for a missing, malformed or truncated file.

Fixtures are generated rather than committed, by `tests/make_fixtures.py`: a
441 Hz sine at 44100 Hz, 100 frames per cycle, as WAV PCM-16, WAV float-32, 16-
and 24-bit AIFF, a truncated WAV, a WAV with a chunk after the audio, and FLAC,
MP3 and Vorbis where `ffmpeg` is on the path. A signal known frame by frame
lets a test say which frame a seek reached.

`tests/run_pd_smoke.py` loads the library into a headless Pd and checks that the
objects create, answer `info`, play and report `eof`. Its patch sends
`open ..., play 1` as a single message with no gap. It skips itself
when no Pd can be found; point it at one with `-DPD_EXECUTABLE=` or the `PD`
environment variable.

CI builds and tests macOS, Linux and Windows.

## Structure

```text
src/audiofile_core.c       opening files, and playing them through miniaudio
src/audiofile_core.h       its interface, and the thread contract
src/af_info.c              the [af.info] class
src/af_play_tilde.c        the [af.play~] class: inlets, outlets, DSP
src/audiofile_setup.c      the library setup
src/af_pdpath.h            path resolution against the patch and search path
src/af_miniaudio.h         the one place miniaudio.h is included
src/miniaudio_impl.c       compiles miniaudio's implementation
```

`audiofile_core` includes no `m_pd.h`, so the tests build and run without Pd.
miniaudio's resource manager reads ahead from disk on a thread of its own, and
the DSP callback pulls finished frames: it never opens files, allocates, locks
or blocks. One thread owns a stream -- Pd's scheduler runs messages and the
perform routine, and a libpd host serialises its calls as libpd requires.

## Technical notes

### Playback

Each stream owns a miniaudio engine created with `noDevice`, so nothing is
opened on the audio hardware and Pd's perform routine pulls frames from it. The
engine's channel count is the object's outlet count and its sample rate is Pd's.
One `ma_sound` stands for the open file, streaming from disk rather than decoded
up front, with spatialization off so that a mono file into a stereo object is
not panned.

Two engine settings are chosen rather than defaulted:

- `periodSizeInFrames` is one Pd block. It also sizes the node graph's caches,
  which a seek plays through: at the 480-frame default a seek is heard seven
  blocks late, at 64 one.
- `preMixStackSizeInBytes` is 16 kB against a default of half a megabyte per
  channel. The graph is one sound into the endpoint, so an eight-channel object
  would otherwise cost four megabytes before a file was open.

`speed` is the sound's pitch, which resamples.

### Seconds inside, frames at the edges

Position and duration are held in seconds. Frames are ambiguous here: a
`ma_sound` counts in frames of the engine's rate whatever the file's own rate
is, so a 48 kHz file in a 44.1 kHz Pd is 20258 frames long inside and 22050
frames long to a patch, and code that mixes the two is wrong in a way that only
shows up on files that need resampling. Seconds are the same number on both
sides. They are converted to frames in two places: seeking, which is what
miniaudio's API takes, and the file's length when it is read at open.

A double holds a second to about 1e-16, which is a five ten-thousandth of a
frame after a day of audio.

### Position

The core counts what it has handed back rather than asking the sound.
`ma_sound_get_cursor_in_pcm_frames` is the read head: the pitch resampler takes
in what it needs to fill a block, so the cursor runs ahead of what has been
heard, further at low speeds. Advancing by the frames returned times the speed
gives the position of the audio just delivered, and wraps with the file.

### Looping, and where a file ends

Sounds are opened with looping already on. The stream fills its pages in
advance, and a page filled while looping was off stops at the end of the file:
switching looping on afterwards leaves a gap of about a block at the first loop
point.

A file played once is therefore stopped here rather than by miniaudio. Each
block is bounded by what is left between the position and the file's length, so
the last block is short by the right amount, `eof` is reported once, and the
play head is left on the end.

A container that records no length cannot be counted down, so such a file is
opened the other way round: looping follows the `loop` message and the end comes
from miniaudio. That costs the seamless loop point and nothing else.

### Seeks

`ma_sound_seek_to_pcm_frame` records a target; the seek happens inside the next
block the engine processes, which is also when the buffered pages are dropped.
That block holds the gap rather than audio, and is reported as no frames rather
than silence, so the position does not advance over frames nobody heard. The
blocks after it come back empty until the new position has been read from disk:
one to three of them.

### Reports

`eof` and `underflow` are edges the perform routine raises in one word and the
Pd clock callback takes, which is why the perform routine sets a clock at all:
`outlet_` calls are not safe from it.

An empty buffer means one of two opposite things. Until the stream has read
enough ahead to say it is running, after a fresh `open` or a seek into a region
not yet on disk, it is filling up, which is not a dropout and is not reported.
After that it is the buffer running dry, reported once per stall.

## Licence

zlib, see [LICENSE](LICENSE). miniaudio is Unlicense OR MIT-0, and including
Pd's `m_pd.h` imposes nothing, so applications that embed Pd can use these
objects whatever their own licence is. greatest is ISC, and is built into the
tests alone.
