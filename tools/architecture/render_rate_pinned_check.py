#!/usr/bin/env python3
"""An offline render in a check must state its sample rate, never inherit the machine's.

WHY. The engine opens the default output device even when rendering to a file — `openAudioDevice`
runs unconditionally — and `baseConfig.sampleRate` is then taken from that device. `--sample-rate`
overrides it, applied after the probe, and that mechanism is correct and has been for a while. What
was wrong is that 42 of the 49 checks that render offline never passed it, so every one of them
produced different bytes depending on what was last plugged in.

That is not theoretical. sampler_vintage "passed for weeks on built-in speakers at 44100 and failed
the first time a Bluetooth device made the default 48000 — with no commit involved, and while the
engine was correct." Six checks were pinned in response to that incident. The other forty-two were
not, which is the recurring shape here: a rule applied where the pain was felt rather than to the
class that can feel it.

44100 IS NOT A PREFERENCE, IT IS THE VALUE THESE ASSERTIONS WERE TUNED AT. The six pre-existing pins
all use SR=44100, and this machine's default device is at 44100, so pinning the rest to it changed no
behaviour — which is the point: the suite staying green is what proves the pin is a no-op and that
the dependence, not the rate, is what was removed. A check that wants a different rate may say so;
what it may not do is stay silent and take the machine's.

THIS CHECKS THE SHAPE, not a list. Any check that invokes `--render` must also pass `--sample-rate`.
There is no allowlist: a new check either states its rate or fails here.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tools")

RENDER = re.compile(r"--render(?:\s|=)")
RATE = re.compile(r"--sample-rate(?:\s|=)")


def main() -> int:
    unpinned, pinned = [], 0
    for name in sorted(os.listdir(TOOLS)):
        if not name.endswith(".sh"):
            continue
        text = open(os.path.join(TOOLS, name), encoding="utf-8").read()
        if not RENDER.search(text):
            continue
        if RATE.search(text):
            pinned += 1
        else:
            unpinned.append(name)

    if unpinned:
        print("FAIL: these render offline without stating a sample rate:")
        for name in unpinned:
            print(f"        tools/{name}")
        print("      The engine opens the default output device even when rendering to a file, so")
        print("      an unpinned render produces different bytes on a different machine — or on the")
        print("      same machine after headphones are plugged in. Pass --sample-rate 44100, which")
        print("      is what the existing assertions are tuned at, or state the rate you need.")
        return 1

    print(f"render_rate_pinned_check: PASS — all {pinned} offline-render check(s) state their rate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
