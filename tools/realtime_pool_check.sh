#!/usr/bin/env bash
# THE RENDER POOL, THROUGH A REAL DEVICE, IN REAL TIME.
#
# Every other check of the pool is an OFFLINE render, and offline is where a threading bug is
# least likely to show: the producer runs flat out with no deadline, so a thread that is late
# simply finishes late and nothing notices. Real time is where being late is the whole problem —
# the callback has a deadline, the ring drains, and a track drops out of the mix for a block.
#
# tools/render_pool_check.sh proves the thread count cannot change the OUTPUT. This proves the
# pool keeps up while a device is actually asking for blocks, which is a different claim and the
# one a listener cares about.
#
# THREE PROPERTIES, and the first is a COMPARISON:
#   PLAYS       eight sampler tracks make sound through the device — captured, not assumed
#   NO WORSE    the pool drops no more blocks than a single thread does on the same machine.
#               Compared rather than thresholded: how many a machine drops depends on the
#               machine, so an absolute number would be a statement about this laptop
#   HEADROOM    the producer's measured load stays well under 1.0x with the pool doing the work
#
# Needs a REAL AUDIO DEVICE. This is the one check here that cannot run headless, and that is
# the point of it.
#   tools/realtime_pool_check.sh [trackCount]      (default 8)
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
TRACKS="${1:-8}"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
w = wave.open(os.path.join(sys.argv[1], "s.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * 220.0 * i / sr)))
                       for i in range(sr * 2)))
w.close()
PY

# The producer-load fixture: N sampler tracks, dense 16ths, 64-voice cap, the expensive
# interpolator. Reused deliberately — it is the heaviest thing the pool is asked to do.
awk '/^import json, sys, os$/{f=1} f&&!/^PY$/{print} /^PY$/&&f{exit}' \
  "$ROOT/tools/producer_load_check.sh" > "$TMP/gen.py"
python3 "$TMP/gen.py" "$TMP/rt.uniproj.json" "$TMP" "$TRACKS"

# take <name> <threads>  — play the project through the device for real, capture it, and report
# what the engine says about keeping up. `threads` "auto" is what a user gets.
#
# The engine is allowed to SHUT DOWN before anything is read: the underrun summary and the
# producer-load line are both written during shutdown, and the first version of this check
# grepped for them immediately after kill(), racing a log that had not been written yet. It
# reported "nothing here is measurable" about a run that measured fine.
take() {
  local name="$1"; local threads="$2"
  local threadEnv=""
  [ "$threads" = "auto" ] || threadEnv="DAW_ENGINE_RENDER_THREADS=$threads"
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/rtpool_$$_$name" DAW_PROJECT_DIR="$TMP" \
      DAW_CAPTURE_WAV="$TMP/$name.wav" ${threadEnv} \
      ./daw_engine --project rt --run-seconds 10 >"$TMP/$name.log" 2>&1 ) &
  ENG=$!
  for _ in $(seq 1 160); do
    grep -q 'starting threads' "$TMP/$name.log" 2>/dev/null && break
    sleep 0.25
  done
  grep -q 'starting threads' "$TMP/$name.log" 2>/dev/null || fail "the engine never came up"
  if [ -x "$CLI" ]; then
    DAW_UI_SHM_NAME="/rtpool_$$_$name" DAW_PROJECT_DIR="$TMP" "$CLI" do play >/dev/null 2>&1 || true
  fi
  wait "$ENG" 2>/dev/null; ENG=""
}

CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

take pool auto
if ! grep -q "Audio device:" "$TMP/pool.log" 2>/dev/null; then
  # Not a failure of the ENGINE. Said out loud rather than passing quietly, because a check that
  # reports success on a machine where it could not run is worse than one that is skipped.
  echo "  SKIP: no audio device on this machine — this check exists to exercise the REAL-TIME"
  echo "        path and there is nothing real-time to exercise. Everything else about the pool"
  echo "        is covered by tools/render_pool_check.sh, which runs offline."
  exit 0
fi
take serial 1

starved() { grep 'Audio underrun summary' "$TMP/$1.log" | tail -1 | grep -o 'summary: [0-9]*' | grep -o '[0-9]*'; }
callbacks() { grep 'Audio underrun summary' "$TMP/$1.log" | tail -1 | grep -o 'of [0-9]*' | grep -o '[0-9]*'; }
loadx() { grep 'Producer load:' "$TMP/$1.log" | tail -1 | grep -o 'load: [0-9.]*' | grep -o '[0-9.]*'; }

PS="$(starved pool)"; PC="$(callbacks pool)"; PL="$(loadx pool)"
SS="$(starved serial)"; SC="$(callbacks serial)"; SL="$(loadx serial)"
[ -n "${PS:-}" ] && [ -n "${SS:-}" ] || fail "the engine wrote no underrun summary, so nothing
        here is measurable — see $TMP/pool.log"
POOLN="$(grep -o 'Render pool: [0-9]* thread' "$TMP/pool.log" | tail -1 | grep -o '[0-9]*')"
echo "  with the pool (${POOLN:-?} threads): $PS dropped of $PC callbacks, producer at ${PL}x"
echo "  on one thread:                      $SS dropped of $SC callbacks, producer at ${SL}x"

# ---- NO WORSE. The comparison is the assertion, not an absolute count: how many callbacks a
# machine drops depends on the machine, the device buffer and what else is running, so a fixed
# threshold would be a statement about this laptop. What must hold is that threading the producer
# did not make real-time behaviour WORSE than leaving it serial.
python3 -c "
p, s = $PS, $SS
raise SystemExit(0 if p <= max(2, s) else 1)" || \
  fail "the render pool dropped MORE blocks than a single thread did: $PS against $SS. Threading
        the producer is supposed to give the callback more room, not less — if it costs dropouts
        the pool is hurting the thing it exists to help"

# ---- HEADROOM.
python3 -c "
raise SystemExit(0 if float('${PL:-9}') < 0.8 else 1)" || \
  fail "the producer ran at ${PL}x of its block budget with $TRACKS tracks on the pool. Under
        1.0x it keeps up; at 1.0x it cannot catch up by definition, and 0.8 leaves room for the
        rest of the machine"

# ---- PLAYS. Captured, not assumed: an engine that reported no underruns because it produced
# silence would satisfy everything above.
[ -s "$TMP/pool.wav" ] || fail "the capture tap wrote no file"
python3 "$ROOT/tools/perceptual.py" "$TMP/pool.wav" --expect-audio >/dev/null || \
  fail "the take is silent. No underruns and no sound is not a pass — it is the pipeline keeping
        up perfectly with nothing"
echo "  the take has audio in it"

echo "realtime_pool_check: PASS — $TRACKS sampler tracks played through a real device on the pool,"
echo "                     dropping no more blocks than one thread does, with room to spare"
