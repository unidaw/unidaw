#!/usr/bin/env bash
# RUN A MULTI-TRACK RENDER UNDER THREADSANITIZER.
#
# The engine is threaded in ways a passing test cannot vouch for: the producer, the consumer, the
# per-track render pool, the master render thread, the underrun reporter and the restart worker
# all touch shared state. tools/render_pool_check.sh proves the OUTPUT does not depend on the
# thread count, which is the property that matters — but a race that happens to produce identical
# bytes on the run you measured is still a race, and it will not stay benign.
#
# This is the tool that sees those. It found two on its first run:
#   - lastAuxOutMask written under controllerMutex and read without it (FIXED: now atomic)
#   - audioCallback assigned by main while the producer and consumer already read it (FILED)
#
# TWO MACOS TOOLCHAIN FIXES ARE BAKED IN, because both cost a build to discover:
#   1. DAW_BUILD_PATCHER_RUST must stay ON. Turning it off to simplify the build removes the
#      staticlib but not the calls to it, and the link fails on undefined symbols.
#   2. -Wl,-no_compact_unwind. Rust, C++, ObjC and TSan each bring their own unwind personality
#      routine, and the linker refuses: "Too many personality routines for compact unwind to
#      encode". Nothing is wrong with the code; compact unwind simply cannot express four.
#
# The Rust staticlib is NOT instrumented, so races inside the patcher kernel are invisible here.
# Everything in the C++ engine is.
#
#   tools/tsan_render.sh [trackCount]        (default 8)
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-tsan"
TRACKS="${1:-8}"

echo "== configuring $BUILD"
cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -Wl,-no_compact_unwind" \
  -DDAW_BUILD_PATCHER_RUST=ON >/dev/null || { echo "configure failed"; exit 2; }

echo "== building (this is a full instrumented build the first time)"
cmake --build "$BUILD" --target daw_engine juce_host_process -j8 >/dev/null || {
  echo "build failed — rerun without >/dev/null to see why"; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
w = wave.open(os.path.join(sys.argv[1], "s.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * 220.0 * i / sr)))
                       for i in range(sr * 2)))
w.close()
PY

# The producer-load fixture: N sampler tracks, dense 16ths, 64-voice cap. Reused deliberately —
# it is the shape that puts the most tracks through the render pool at once, which is where a
# race between per-track work would show.
awk '/^import json, sys, os$/{f=1} f&&!/^PY$/{print} /^PY$/&&f{exit}' \
  "$ROOT/tools/producer_load_check.sh" > "$TMP/gen.py"
python3 "$TMP/gen.py" "$TMP/t.uniproj.json" "$TMP" "$TRACKS"

echo "== rendering $TRACKS tracks under TSan"
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/tsan_$$" \
    TSAN_OPTIONS="halt_on_error=0" \
    ./daw_engine --project t --render t --sample-rate 44100 --run-seconds 6 --block-size 256 \
    >"$TMP/tsan.log" 2>&1 )

REPORT="$ROOT/build-tsan/tsan-report.txt"
cp "$TMP/tsan.log" "$REPORT"
COUNT="$(grep -c 'WARNING: ThreadSanitizer' "$TMP/tsan.log" 2>/dev/null || true)"
echo
echo "== ${COUNT:-0} race report(s); full log at $REPORT"
grep 'SUMMARY: ThreadSanitizer' "$TMP/tsan.log" 2>/dev/null | sed 's/.*ThreadSanitizer: /   /' |
  sort | uniq -c
echo
echo "Read the full stacks in the log: each report names the two accesses, the thread that made"
echo "each, and the allocation site of what they raced on. A report is not automatically a bug —"
echo "but every one of them is a claim about the code that has to be answered."
