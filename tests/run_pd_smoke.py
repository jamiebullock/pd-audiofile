#!/usr/bin/env python3
"""Load the library into a headless Pd and check the objects work.

Exits 77 when the fixtures or Pd are missing, which CMake reads as a skip.

usage: run_pd_smoke.py <directory holding the external> <fixture directory>
                       [path to pd]

Part of pd-audiofile

SPDX-FileCopyrightText: 2026 Jamie Bullock
SPDX-License-Identifier: Zlib
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# What tests/smoke.pd should print.
EXPECTED = [
    "INFO: info 44100 0.5 1 s16",
    "PLAY: info 44100 0.5 1 s16",
    "PLAY: pos",
    "PLAY: eof",
]

# `PLAY: error` is here because `open ..., play 1` in one message once
# reported a dropout before a frame had been delivered.
UNEXPECTED = [
    "INFO: error",
    "PLAY: error",
    "couldn't create",
    "no method for",
]


def find_pd(explicit):
    """Return the path to pd, and True if it was asked for by argument or by
    the PD environment variable rather than found by searching. A pd that was
    asked for and is not there is an error; finding none is a reason to skip.
    """
    if explicit:
        return explicit, True
    if os.environ.get("PD"):
        return os.environ["PD"], True

    found = shutil.which("pd")
    if found:
        return found, False

    patterns = []
    if sys.platform == "darwin":
        patterns = ["/Applications/Pd*.app/Contents/Resources/bin/pd"]
    elif sys.platform.startswith("win"):
        patterns = [
            r"C:\Program Files\Pd\bin\pd.exe",
            r"C:\Program Files (x86)\Pd\bin\pd.exe",
        ]
    else:
        patterns = ["/usr/bin/pd", "/usr/local/bin/pd"]

    for pattern in patterns:
        for candidate in sorted(glob.glob(pattern), reverse=True):
            if os.path.isfile(candidate):
                return candidate, False
    return None, False


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    extdir = os.path.abspath(argv[1])
    fixdir = os.path.abspath(argv[2])
    if not os.path.isfile(os.path.join(fixdir, "sine_mono_44100_s16.wav")):
        print("no fixtures in %s: skipping" % fixdir, file=sys.stderr)
        return 77

    pd_bin, was_asked_for = find_pd(argv[3] if len(argv) > 3 else None)

    if not pd_bin or not os.path.isfile(pd_bin):
        if was_asked_for:
            print("the Pd asked for is not there: %s" % pd_bin, file=sys.stderr)
            return 1
        print("no Pd found: skipping", file=sys.stderr)
        return 77

    print("pd: %s" % pd_bin)

    command = [
        pd_bin,
        "-nogui",
        "-noaudio",
        "-stderr",
        "-path", extdir,
        "-path", fixdir,
        "-lib", "audiofile",
        os.path.join(HERE, "smoke.pd"),
    ]

    with tempfile.TemporaryDirectory() as cwd:
        try:
            done = subprocess.run(command, cwd=cwd, timeout=60,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
            log = done.stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as expired:
            log = (expired.stdout or b"").decode("utf-8", "replace")
            print(log)
            print("Pd did not quit within 60s", file=sys.stderr)
            return 1
        except OSError as exc:
            print("could not run %s: %s" % (pd_bin, exc), file=sys.stderr)
            return 1

    print(log)

    if done.returncode != 0:
        print("Pd exited with status %d" % done.returncode, file=sys.stderr)
        return 1

    print("checking the smoke patch's output:")

    failed = False
    for wanted in EXPECTED:
        if wanted in log:
            print("  ok    %s" % wanted)
        else:
            print("  FAIL  expected: %s" % wanted)
            failed = True
    for unwanted in UNEXPECTED:
        if unwanted in log:
            print("  FAIL  unexpected: %s" % unwanted)
            failed = True
        else:
            print("  ok    no %s" % unwanted)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
