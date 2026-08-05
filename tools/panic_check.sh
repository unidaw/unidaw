#!/usr/bin/env bash
# Checks PANIC (UiCommandType 52): all sound off, and it STAYS off.
#
# Stop halts, rewinds and releases held keyjazz notes — correct, but not a panic: it
# cannot reach a plugin's own ringing voices, a sequencer note whose note-off has not been
# reached, or a generator mid-phrase. Panic sends CC120 (all-sound-off) AND CC123
# (all-notes-off) on every MIDI channel to every hosted plugin and drops the engine's note
# bookkeeping. CC120 is the one that matters: CC123 merely releases notes and lets a pad
# ring out, which is not what a person reaching for panic wants.
#
# The stimulus is maximal (six Zebra2 tracks, two of them generator-driven) played for a
# few seconds, so real sequencer notes AND a generator mid-phrase are sounding when panic
# lands. PASS requires audio before, silence right after, and silence that PERSISTS —
# sound coming back on its own is exactly what makes a panic button untrustworthy.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built, and
# Zebra2 (the maximal preset's instrument).
#   tools/panic_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
SHM="/panic_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }
[ -d "/Library/Audio/Plug-Ins/VST3/Zebra2.vst3" ] || { echo "SKIP: Zebra2 not installed"; exit 0; }

. "$ROOT/tools/lib/engine_wait.sh"

TMP="$(mktemp -d)"
ENG=""
# THE EVIDENCE USED TO BE DELETED ON THE WAY OUT. `rm -rf "$TMP"` ran unconditionally, and it ran
# BEFORE the PASS/FAIL line — so the one run whose capture and engine log you need was the one run
# that destroyed them. This check failed once inside a full ctest on 2026-08-05, passed standalone
# in 17.8s, and left nothing behind to say which of its two very different failures it had been:
# "the panic did not work" or "nothing was sounding, so the check proved nothing". Those want
# opposite responses, and without the wav there is no way to tell them apart afterwards.
#
# The trap also STOPS THE ENGINE, which nothing did before: there was no trap at all, so a run
# killed by a ctest timeout orphaned an engine holding a real audio device.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
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

( cd "$BUILD" && DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$ROOT/presets/projects" \
    DAW_CAPTURE_WAV="$TMP/p.wav" DAW_CAPTURE_SECONDS=14 \
    ./daw_engine --run-seconds 17 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
# WAIT FOR THE ENGINE, DO NOT BET 2.5s ON IT. The assertions below read fixed windows of the
# capture (4-7s, 9-11s, 11-13s), so every second the boot runs long shifts what is inside them —
# and a boot that overruns puts the "before" window in the silence BEFORE playback started, which
# reports as "nothing was sounding" and reads exactly like a broken fixture. On a loaded machine
# this engine loads six Zebra2 instances, which is the slowest boot in the suite.
# The PATTERN matters: this engine boots with NO project and is given one below, so the default
# "project loaded" condition would never fire and the check would fail before it started. What it
# needs is the command thread, which is what makes `do load` land at all.
wait_for_boot "$TMP/engine.log" "$ENG" 60 "UI: command thread started"
DAW_UI_SHM_NAME="$SHM" "$CLI" do load maximal --force >/dev/null 2>&1 || true
# The load is what instantiates the six plugins, so this is the long one. Waiting on the engine's
# own event beats a second guess about how long that takes.
wait_for_event "$TMP/engine.log" "project.load" 40 >/dev/null 2>&1 || true
DAW_UI_SHM_NAME="$SHM" "$CLI" do play --force >/dev/null 2>&1 || true
# A REAL DURATION, not a guess: five seconds of playing is the stimulus this check needs in the
# capture, so it stays a sleep. Everything above it is now a wait, which is what keeps these five
# seconds landing where the analysis expects them.
sleep 5
DAW_UI_SHM_NAME="$SHM" "$CLI" do panic --force >/dev/null 2>&1 || true
wait "$ENG" 2>/dev/null || true
ENG=""

ok=1
grep -q "PANIC" "$TMP/engine.log" || { echo "  FAIL: the engine never saw the panic command"; ok=0; }
[ -s "$TMP/p.wav" ] || { echo "  FAIL (setup): no capture was written"; tail -3 "$TMP/engine.log" | sed 's/^/    /'; ok=0; }

if [ "$ok" = "1" ]; then
  set +e
  python3 - "$TMP/p.wav" <<'PY'
import sys, wave, numpy as np
w = wave.open(sys.argv[1], 'rb'); sr = w.getframerate(); ch = w.getnchannels()
d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)/32768.0
if ch > 1: d = d.reshape(-1, ch).mean(axis=1)
def rms(a, b):
    seg = d[int(a*sr):int(b*sr)]
    return float(np.sqrt((seg**2).mean())) if seg.size else 0.0
before, after, tail = rms(4, 7), rms(9, 11), rms(11, 13)
print(f"  rms before panic={before:.5f}  after={after:.5f}  tail={tail:.5f}")
ok = True
setup = False
if before < 1e-4:
    print("  nothing was sounding before the panic, so this run cannot answer the question")
    setup = True
    ok = False
if after > 1e-4:
    print("  FAIL: sound survived the panic")
    ok = False
if tail > 1e-4:
    print("  FAIL: sound CAME BACK after the panic")
    ok = False
if ok:
    print("  sound stopped on panic and stayed stopped")
# EXIT 2 IS "THIS RUN COULD NOT ANSWER", distinct from 1 ("panic did not work"). The shell decides
# which it is, because deciding needs the engine log and this block only has the wav.
raise SystemExit(0 if ok else (2 if setup else 1))
PY
  rc=$?
  [ "$rc" = "0" ] || ok=0
  # SAY WHY IT WAS SILENT — AND STILL FAIL. A first version of this excused the run as BLOCKED
  # when the capture came back empty, on the theory that a device which cannot deliver the
  # stimulus makes the question unanswerable. That reasoning is only sound when the second signal
  # CANNOT be caused by what is under test, and here it can: the empty capture on this machine is
  # the PRODUCER having no block ready for 1427 of 1468 callbacks, which is an engine failure
  # under six hosted synths and precisely the kind of regression this suite exists to catch.
  # Excusing it would have turned that green.
  #
  # The one case that genuinely is the machine — a device that never runs a callback at all — is
  # already named by capture_diagnosis, and it is left FAILING too rather than excused, because a
  # dead output and a broken audio thread print the same thing from here.
  if [ "$rc" = "2" ]; then
    echo "  $(capture_diagnosis "$TMP/engine.log")"
  fi
fi

# NO `rm -rf "$TMP"` HERE — the trap owns cleanup now, and it keeps the directory when the run
# failed. Removing it here would delete the capture before the trap could copy it, which is what
# this line used to do.
[ "$ok" = "1" ] && echo "panic_check: PASS — panic cuts all sound and it stays cut" \
                || { echo "panic_check: FAIL"; exit 1; }
