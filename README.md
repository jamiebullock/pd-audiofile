# pd-audiofile

A sound-file library for [Pure Data](https://puredata.info), built on
[miniaudio](https://github.com/mackron/miniaudio).

| Object | Kind | Does |
| --- | --- | --- |
| `[af.info]` | control | reports sample rate, duration in seconds, channels and sample format from a file's header |
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
| `info` | samplerate, duration in seconds, channels, format-symbol | on `open` |
| `error` | symbol | the file could not be read, with a short reason; also printed to the Pd console |

`[openpanel]` goes through `[open $1(`, as it does into `[readsf~]`.

### `[af.play~]`

```pd
[af.play~ <channels>]
```

Default: 2 channels
Outlets: one signal per channel, then a message outlet. A file with a different channel
count is mixed.

| Message in | Argument | Meaning |
| --- | --- | --- |
| `open <path>` | symbol | open the file and report `info`; does not start playback |
| `info` | | report `info` for the file already open |
| `play <0/1>` | float | start and stop |
| `loop <0/1>` | float | return to the start on reaching the end |
| `autorestart <0/1>` | float | whether `play 1` rewinds a file that has reached its end; on by default |
| `speed <f>` | float | playback rate; 1 is the file's own rate |
| `pos <f>` | float | seek, in seconds from the start |
| `getpos` | | report `pos` once |
| `close` | | release the file |

| Selector out | Arguments | When |
| --- | --- | --- |
| `info` | samplerate, duration in seconds, channels, format-symbol | after `open`, and on `info` |
| `pos` | seconds | on `getpos` |
| `eof` | | on reaching the end with looping off |
| `error` | symbol | any failure, with a short reason; also printed to the Pd console |

`[openpanel]` goes through `[open $1(` here too, and a toggle through
`[play $1(`. `info` reports the same four elements `[af.info]` does.

Rules:

- `open` replaces the file being played and stops playback. A failed `open`
  leaves the file that was open in place.
- A seek past the end clamps. With looping off reads as end of file.
- A file at another sample rate is resampled to Pd's rate, and `speed` multiplies
  that ratio. At speed 2 a file plays twice as fast and an octave higher.
- Negative speed and reverse playback are not supported.
- Silence is output for 1-2 blocks whilst seek buffers
- Buffer underflow outputs silence and reports `error underflow` once per stall
  rather than once per block. 

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The build fetches dependencies. This can be skipped by specifying directories:

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

Writes an `audiofile/` directory holding the external, the help patches and
the meta patch.
[deken](https://github.com/pure-data/deken) package layout is followed.

## Embedding: sharing miniaudio with a host application

miniaudio's implementation is compiled in exactly one unit, so an
external loaded into an application that already compiles miniaudio must not
compile a second copy. This can be controlled with the
`AUDIOFILE_USE_HOST_MINIAUDIO` flag:

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

`tests/test_core.c` runs the core under greatest.
`tests/run_pd_smoke.py` loads the library into a headless Pd and checks the
objects create, answer and report `eof`; point it at a Pd with
`-DPD_EXECUTABLE=` or the `PD` environment variable.

`tests/make_fixtures.py` writes the fixtures, one sine at 100 frames per cycle
in each format the tests read, and needs `soundfile`. Without it, or without a
Pd, the tests skip rather than fail. A Python that refuses `pip install`, as
Homebrew's does, wants a virtual environment and `-DPython3_EXECUTABLE=`.

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

## Licence

zlib, see [LICENSE](LICENSE). miniaudio is Unlicense OR MIT-0, and including
Pd's `m_pd.h` imposes nothing, so applications that embed Pd can use these
objects whatever their own licence is. greatest is ISC, and is built into the
tests alone.
