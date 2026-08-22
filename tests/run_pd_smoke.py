#!/usr/bin/env python3
"""Load the library into a headless Pd and check the objects work there.

The C tests exercise the core with no Pd anywhere near it. This checks the part
they cannot: that the binary loads as `-lib audiofile`, that the classes create,
and that they answer. Windows is where loading is most likely to break, which is
why this is Python rather than a shell script.

Exits 77 when no Pd can be found, which CMake reads as a skip.

usage: run_pd_smoke.py <directory holding the external> <fixture directory>
                       [path to pd]

Part of pd-audiofile. SPDX-License-Identifier: Zlib
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# What tests/smoke.pd should print. The `error` line is the interesting one:
# `open …, run 1` in a single message used to report a dropout before a frame
# had been delivered, and nothing in the C tests would have caught it in Pd.
EXPECTED = [
    "INFO: 44100 22050 1 s16",
    "PLAY: info 44100 22050 1 s16",
    "PLAY: pos",
    "PLAY: eof",
    # A bare symbol, which is what [openpanel] emits, on both classes.
    "SYMINFO: 44100 22050 1 s16",
    "SYMPLAY: info 44100 22050 1 s16",
]

UNEXPECTED = [
    "PLAY: error",
    "SYMPLAY: error",
    "couldn't create",
    "no method for",
]


def find_pd(explicit):
    """Returns (path, explicit). An explicit path that is not there is an
    error rather than a reason to skip: someone meant that one."""
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
        # -noaudio still runs the scheduler and the DSP chain, off the system
        # clock, which is all the patch needs.
        "-noaudio",
        "-stderr",
        "-path", extdir,
        "-path", fixdir,
        "-lib", "audiofile",
        os.path.join(HERE, "smoke.pd"),
    ]

    # From a directory of its own: -lib tries the working directory first, and
    # a relative dlopen is refused outright by a hardened macOS process.
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

    # The patch quits Pd itself, so a non-zero status is a crash on the way
    # out, which is one of the things loading a library here is meant to catch.
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
