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
# FOUR PROPERTIES, and two of them are COMPARISONS:
#   PLAYS       eight sampler tracks make sound through the device — captured, not assumed
#   DOES ITS JOB  the pool's producer LOAD is materially below one thread's on the same run.
#                 Asserted on load and not on dropouts: dropout counts at these sizes measure
#                 the laptop (3/4/2/6 against 78/4/0/3 across four runs), while the load is
#                 stable to a few percent because it is a property of the engine
#   ENGAGED       the adaptive rule actually turned the pool on, or the comparison is one
#                 thread against one thread and passes because nothing differs
#   HEADROOM    the producer's measured load stays well under 1.0x with the pool doing the work
#
# Needs a REAL AUDIO DEVICE. This is the one check here that cannot run headless, and that is
# the point of it.
# TWENTY-FOUR TRACKS BY DEFAULT, because the pool now engages on the WORK and at eight tracks it
# correctly declines to: one thread spends 0.18x of the budget there and waking seven workers
# every block costs more than it saves. Measured across four runs at eight tracks, the pool
# dropped 4/0/2/7 callbacks against one thread's 0/3/0/0 — which is what taught the engine to
# engage on load rather than on isolation. A check at eight tracks would now be measuring the
# adaptive rule declining, not the pool working.
#
#   tools/realtime_pool_check.sh [trackCount]      (default 24)
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
. "$ROOT/tools/lib/engine_wait.sh"   # for capture_diagnosis
TRACKS="${1:-24}"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
# stop_engine, not a bare kill: it SIGTERMs, waits 10s, ESCALATES to SIGKILL and SAYS SO.
# A hung engine that ignores SIGTERM is left running by a bare kill, and ctest then waits
# about 1000s for it after timing the check out — measured across 18 runs, perfectly
# correlated with the timeout count. The escalation notice is also the diagnostic: "engine
# N ignored SIGTERM for 10s" names the hang instead of leaving it to be inferred.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
# KEEP THE EVIDENCE WHEN IT FAILS, then clean up exactly as before. The failure messages in these
# checks point at logs inside $TMP, and cleanup() removes $TMP — so the one run whose log you need
# is the one run that deletes it. This wraps the existing cleanup rather than editing it: cleanup
# still runs, still stops engines, still removes the directory.
keep_evidence_then() {
  local rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
    local dest="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  "$@"
  exit $rc
}
trap 'keep_evidence_then cleanup' EXIT
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
  ( cd "$BUILD" && exec env DAW_UI_SHM_NAME="/rtpool_$$_$name" DAW_PROJECT_DIR="$TMP" \
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
# THE POOL HAS TO HAVE ENGAGED, or this is a comparison of serial against serial that passes
# because nothing differs. "auto" is the shipping configuration and it engages on the WORK, so a
# fixture too light to provoke it makes the whole check vacuous — exactly the failure mode this
# suite keeps finding in its own fixtures.
grep -q '"event":"producer.pool_engaged"' "$TMP/pool.log" || \
  fail "the pool never engaged with $TRACKS tracks, so the run compared one thread against one
        thread. Either the fixture is too light to need the pool, or the adaptive rule is not
        engaging when it should — check producer.load in $TMP/pool.log"
echo "  with the pool (${POOLN:-?} threads): $PS dropped of $PC callbacks, producer at ${PL}x"
echo "  on one thread:                      $SS dropped of $SC callbacks, producer at ${SL}x"

# ---- THE POOL DOES ITS JOB, measured as LOAD and not as dropouts.
#
# Dropouts were the obvious assertion and they are not assertable. Across runs at 24 tracks the
# pool dropped 3/4/2/6 callbacks and one thread dropped 78/4/0/3 — the 78 an outlier from
# whatever else the machine was doing, and the rest indistinguishable noise. Neither
# configuration is genuinely starving at this size, so the counts measure the laptop.
#
# The LOAD is stable to a few percent across the same runs (pool 0.09-0.13x, serial 0.46-0.52x)
# because it is a property of the engine rather than of the moment. So that is what is asserted,
# and the dropout counts are printed for a human to look at.
python3 -c "
p, s = float('${PL:-9}'), float('${SL:-0}')
raise SystemExit(0 if s > 0 and p < s * 0.7 else 1)" || \
  fail "the pool did not reduce the producer's load: ${PL}x against ${SL}x on one thread. With
        $TRACKS sampler tracks the work is well past the point where spreading it should help,
        so no improvement means the parallel group is not getting the tracks"

# A SYSTEMATIC collapse would still show, and this is deliberately loose enough that noise cannot
# trip it: a pool that dropped tens of blocks where one thread dropped none is broken, not
# unlucky.
python3 -c "
p, s = $PS, $SS
raise SystemExit(0 if p <= s + 25 or p <= 25 else 1)" || \
  fail "the pool dropped $PS callbacks against one thread's $SS. That is far past the run-to-run
        noise this check tolerates on purpose — something is starving the callback"

# ---- HEADROOM.
python3 -c "
raise SystemExit(0 if float('${PL:-9}') < 0.8 else 1)" || \
  fail "the producer ran at ${PL}x of its block budget with $TRACKS tracks on the pool. Under
        1.0x it keeps up; at 1.0x it cannot catch up by definition, and 0.8 leaves room for the
        rest of the machine"

# ---- PLAYS. Captured, not assumed: an engine that reported no underruns because it produced
# silence would satisfy everything above.
[ -s "$TMP/pool.wav" ] || fail "the capture tap wrote no file. Specifically:
        $(capture_diagnosis "$TMP/pool.log")
        Full log: $TMP/pool.log"
python3 "$ROOT/tools/perceptual.py" "$TMP/pool.wav" --expect-audio >/dev/null || \
  fail "the take is silent. No underruns and no sound is not a pass — it is the pipeline keeping
        up perfectly with nothing"
echo "  the take has audio in it"

echo "realtime_pool_check: PASS — $TRACKS sampler tracks played through a real device, the pool"
echo "                     engaged on its own, and it cut the producer's load from ${SL}x to ${PL}x"
