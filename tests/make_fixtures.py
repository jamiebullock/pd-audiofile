#!/usr/bin/env python3
"""Generate the test fixtures.

Part of pd-audiofile

SPDX-FileCopyrightText: 2026 Jamie Bullock
SPDX-License-Identifier: Zlib
"""

import argparse
import os
import struct
import sys

import numpy
import soundfile

RATE = 44100
FREQ = 441.0
FRAMES = 22050
AMP = 0.5


def sine(frames, rate=RATE, channels=1, freq=FREQ, amp=AMP):
    """Return `frames` samples of a sine, stored (frames,) for one channel
    and (frames, channels) for multiple channels. 
    The second channel, if present, is inverted.
    """
    v = amp * numpy.sin(2.0 * numpy.pi * freq * numpy.arange(frames) / rate)
    if channels == 1:
        return v

    out = numpy.repeat(v[:, None], channels, axis=1)
    out[:, 1] = -v
    return out


def report(path):
    print("  %-34s %8d bytes" % (os.path.basename(path), os.path.getsize(path)))


def write(path, samples, rate=RATE, **kwargs):
    soundfile.write(path, samples, rate, **kwargs)
    report(path)


def write_bytes(path, blob):
    with open(path, "wb") as f:
        f.write(blob)
    report(path)


def trailing_chunk(path, samples):
    """Write a WAV with a LIST/INFO chunk after the audio, which a parser
    that takes everything past the header as samples measures as too long."""
    soundfile.write(path, samples, RATE, subtype="PCM_16")
    with open(path, "rb") as f:
        wav = f.read()

    info = b"INFO" + b"ISFT" + struct.pack("<I", 22) + b"pd-audiofile fixtures\x00"
    blob = wav + b"LIST" + struct.pack("<I", len(info)) + info
    write_bytes(path, blob[:4] + struct.pack("<I", len(blob) - 8) + blob[8:])


def truncated(path, samples, keep=0.4):
    """Write a WAV holding `keep` of its audio, with a header that
    declares the full length."""
    soundfile.write(path, samples, RATE, subtype="PCM_16")
    with open(path, "rb") as f:
        blob = f.read()

    write_bytes(path, blob[:len(blob) - int(len(samples) * 2 * (1.0 - keep))])


def garbage(path):
    write_bytes(path, b"this is not an audio file, and says so in ASCII\n" * 8)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("outdir", nargs="?", default="fixtures")
    args = ap.parse_args()

    out = args.outdir
    os.makedirs(out, exist_ok=True)
    j = lambda name: os.path.join(out, name)

    mono = sine(FRAMES)
    stereo = sine(FRAMES, channels=2)
    mono48 = sine(FRAMES, rate=48000, freq=480.0)   # a 100-frame cycle at 48000

    print("writing fixtures to %s" % os.path.abspath(out))

    write(j("sine_mono_44100_s16.wav"), mono, subtype="PCM_16")
    write(j("sine_mono_44100_f32.wav"), mono, subtype="FLOAT")
    write(j("sine_stereo_44100_s16.wav"), stereo, subtype="PCM_16")
    write(j("sine_mono_48000_s16.wav"), mono48, rate=48000, subtype="PCM_16")
    write(j("sine_mono_44100_s16.aiff"), mono, format="AIFF", subtype="PCM_16")
    write(j("sine_mono_44100_s24.aiff"), mono, format="AIFF", subtype="PCM_24")
    write(j("sine_mono_44100.flac"), mono, format="FLAC", subtype="PCM_16")

    for name, fmt, subtype in (("sine_mono_44100.ogg", "OGG", "VORBIS"),
                               ("sine_mono_44100.mp3", "MP3", "MPEG_LAYER_III")):
        if soundfile.check_format(fmt, subtype):
            write(j(name), mono, format=fmt, subtype=subtype)
        else:
            print("  skipping %s: libsndfile %s cannot write it"
                  % (name, soundfile.__libsndfile_version__))

    trailing_chunk(j("trailing_chunk.wav"), mono)
    truncated(j("truncated.wav"), mono)
    garbage(j("garbage.wav"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
