#!/usr/bin/env python3
"""Generate the test fixtures.

Written rather than committed, so that what the tests assert about a file can
be read here instead of guessed at from a binary. The signal is a 441 Hz sine
at 44100 Hz -- exactly 100 samples per cycle -- so the expected value of frame
n is sin(2*pi*n/100) whatever the container, and a test can check where a seek
landed by looking at the phase.

WAV and AIFF are written here, by hand, because they are what the header
parsing has to get right and because a stdlib module that writes them is not
something to rely on any more. FLAC, MP3 and Vorbis are handed to ffmpeg when
it is on the path; the C tests skip whatever is missing.

Part of pd-audiofile

SPDX-FileCopyrightText: 2026 Jamie Bullock
SPDX-License-Identifier: Zlib
"""

import argparse
import math
import os
import shutil
import struct
import subprocess
import sys

RATE = 44100
FREQ = 441.0          # exactly RATE / 100
FRAMES = 22050        # half a second, 220.5 cycles
AMP = 0.5


def sine(frames, rate=RATE, channels=1, freq=FREQ, amp=AMP):
    """Interleaved floats.

    Channel 1, if present, is inverted -- the channels are mirrored, in
    anti-phase. That is what lets test_channels assert the two are kept apart
    rather than smeared together, by checking that they sum to zero.

    Be aware of it when using these fixtures for anything else: mixing
    sine_stereo_44100_s16.wav down to mono cancels to exact silence, which
    looks like a decoding failure and is not one.
    """
    out = []
    for n in range(frames):
        v = amp * math.sin(2.0 * math.pi * freq * n / rate)
        for c in range(channels):
            out.append(-v if c == 1 else v)
    return out


def to_s16(samples):
    return b"".join(struct.pack("<h", max(-32768, min(32767, int(round(v * 32767)))))
                    for v in samples)


def to_s16_be(samples):
    return b"".join(struct.pack(">h", max(-32768, min(32767, int(round(v * 32767)))))
                    for v in samples)


def to_s24_be(samples):
    out = bytearray()
    for v in samples:
        i = max(-8388608, min(8388607, int(round(v * 8388607))))
        if i < 0:
            i += 1 << 24
        out += bytes(((i >> 16) & 0xFF, (i >> 8) & 0xFF, i & 0xFF))
    return bytes(out)


def to_f32(samples):
    return b"".join(struct.pack("<f", v) for v in samples)


def riff(chunks):
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def chunk(tag, payload):
    pad = b"\x00" if len(payload) % 2 else b""
    return tag + struct.pack("<I", len(payload)) + payload + pad


def wav_pcm16(path, samples, channels, rate=RATE):
    data = to_s16(samples)
    fmt = struct.pack("<HHIIHH", 1, channels, rate,
                      rate * channels * 2, channels * 2, 16)
    write(path, riff([chunk(b"fmt ", fmt), chunk(b"data", data)]))


def wav_float32(path, samples, channels, rate=RATE):
    data = to_f32(samples)
    fmt = struct.pack("<HHIIHH", 3, channels, rate,
                      rate * channels * 4, channels * 4, 32)
    write(path, riff([chunk(b"fmt ", fmt), chunk(b"data", data)]))


def wav_trailing_chunk(path, samples, channels, rate=RATE):
    """A LIST/INFO chunk after the audio. A parser that assumes `data` is last,
    or that stops looking once it has found it, gets the frame count wrong."""
    data = to_s16(samples)
    fmt = struct.pack("<HHIIHH", 1, channels, rate,
                      rate * channels * 2, channels * 2, 16)
    info = b"INFO" + chunk(b"ISFT", b"pd-audiofile fixtures\x00")
    write(path, riff([chunk(b"fmt ", fmt), chunk(b"data", data),
                      chunk(b"LIST", info)]))


def wav_truncated(path, samples, channels, rate=RATE, keep=0.4):
    """The header claims the full length; the file stops part way through the
    audio. Decoders differ on whether this is an error or a short file, so the
    test asserts only that it neither crashes nor reports the full length."""
    data = to_s16(samples)
    fmt = struct.pack("<HHIIHH", 1, channels, rate,
                      rate * channels * 2, channels * 2, 16)
    full = riff([chunk(b"fmt ", fmt), chunk(b"data", data)])
    cut = len(full) - int(len(data) * (1.0 - keep))
    write(path, full[:cut])


def extended80(rate):
    """IEEE 754 80-bit extended, which is how AIFF records a sample rate."""
    if rate == 0:
        return b"\x00" * 10
    exponent = 0
    mantissa = float(rate)
    while mantissa < (1 << 63):
        mantissa *= 2.0
        exponent -= 1
    while mantissa >= (1 << 64):
        mantissa /= 2.0
        exponent += 1
    return struct.pack(">HQ", 16383 + 63 + exponent, int(mantissa))


def aiff(path, samples, channels, bits, rate=RATE):
    if bits == 16:
        data = to_s16_be(samples)
    elif bits == 24:
        data = to_s24_be(samples)
    else:
        raise ValueError("aiff fixture: 16 or 24 bits")

    frames = len(samples) // channels
    comm = struct.pack(">hIh", channels, frames, bits) + extended80(rate)
    ssnd = struct.pack(">II", 0, 0) + data

    body = b"AIFF" + be_chunk(b"COMM", comm) + be_chunk(b"SSND", ssnd)
    write(path, b"FORM" + struct.pack(">I", len(body)) + body)


def be_chunk(tag, payload):
    pad = b"\x00" if len(payload) % 2 else b""
    return tag + struct.pack(">I", len(payload)) + payload + pad


def garbage(path):
    write(path, b"this is not an audio file, and says so in ASCII\n" * 8)


def write(path, blob):
    with open(path, "wb") as f:
        f.write(blob)
    print("  %-34s %8d bytes" % (os.path.basename(path), len(blob)))


def encode(ffmpeg, src, dst, *args):
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", src]
    cmd += list(args) + [dst]
    try:
        subprocess.run(cmd, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, OSError) as exc:
        print("  skipping %s (%s)" % (os.path.basename(dst), type(exc).__name__))
        # A failed run can leave a stub behind, and a stub reads as a fixture.
        if os.path.exists(dst):
            os.remove(dst)
        return False
    print("  %-34s %8d bytes" % (os.path.basename(dst), os.path.getsize(dst)))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("outdir", nargs="?", default="fixtures")
    args = ap.parse_args()

    out = args.outdir
    os.makedirs(out, exist_ok=True)
    j = lambda name: os.path.join(out, name)

    mono = sine(FRAMES, channels=1)
    stereo = sine(FRAMES, channels=2)
    mono48 = sine(FRAMES, rate=48000, channels=1, freq=480.0)   # 100 samples/cycle again

    print("writing fixtures to %s" % os.path.abspath(out))

    wav_pcm16(j("sine_mono_44100_s16.wav"), mono, 1)
    wav_float32(j("sine_mono_44100_f32.wav"), mono, 1)
    # Mirrored channels: see sine(). A mono downmix of this file is silence.
    wav_pcm16(j("sine_stereo_44100_s16.wav"), stereo, 2)
    wav_pcm16(j("sine_mono_48000_s16.wav"), mono48, 1, rate=48000)
    wav_trailing_chunk(j("trailing_chunk.wav"), mono, 1)
    wav_truncated(j("truncated.wav"), mono, 1)

    aiff(j("sine_mono_44100_s16.aiff"), mono, 1, 16)
    aiff(j("sine_mono_44100_s24.aiff"), mono, 1, 24)

    garbage(j("garbage.wav"))

    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        print("  ffmpeg not found: skipping FLAC, MP3 and Vorbis fixtures")
        return 0

    src = j("sine_mono_44100_f32.wav")
    encode(ffmpeg, src, j("sine_mono_44100.flac"), "-c:a", "flac")
    encode(ffmpeg, src, j("sine_mono_44100.mp3"), "-c:a", "libmp3lame", "-b:a", "192k")
    if not encode(ffmpeg, src, j("sine_mono_44100.ogg"), "-c:a", "libvorbis", "-q:a", "6"):
        # ffmpeg's own Vorbis encoder, for builds without libvorbis. If that
        # is missing too, the tests skip Vorbis.
        encode(ffmpeg, src, j("sine_mono_44100.ogg"),
               "-c:a", "vorbis", "-strict", "-2")
    return 0


if __name__ == "__main__":
    sys.exit(main())
