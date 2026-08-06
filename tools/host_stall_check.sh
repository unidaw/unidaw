#!/usr/bin/env bash
# ONE STUCK PLUGIN HOST MUST NOT SILENCE THE WHOLE SONG — and today it does not.
#
# READ THIS BEFORE ASSUMING IT GUARDS A FIX. It does not. It records a property that CURRENTLY
# HOLDS, and it was written while chasing a different failure it turned out not to reproduce.
# Keeping it is worth a paragraph of honesty:
#
# The reported bug is "add a synth and the whole thing goes quiet for 30 seconds". Measured with
# six Zebra2 instances, the engine gates on the MINIMUM completed block across every host and one
# host sat frozen while the others moved on:
#
#   producer stall (inFlight) next=59 minCompleted=54 playback=90 extra=5
#                             hosts=[0:54,1:58,2:58,3:58,4:58,5:58]
#
# SIGSTOPping a host is the obvious way to make that deterministic, and it is what this check
# does. IT DOES NOT REPRODUCE THE BUG: with a host frozen, the producer does NOT stay gated and
# the song keeps playing — both before and after the candidate fix I wrote for it, which is how I
# learned the fix was unproven and reverted it. Something already handles a wholly stopped host;
# whatever freezes one host at block 54 in the real failure is subtler than "the process stopped".
#
# So this is a PROPERTY test, not a regression test for a fix: if a future change makes a frozen
# host able to halt the transport, this goes red. That is worth having. What it must never be
# read as is evidence that the reported silence is fixed — it is not fixed, and the stall
# diagnostics in apps/engine_producer_thread.cpp (which now report the REAL playback block and
# per-host progress, both of which were previously a hardcoded 0) are what will catch it.
#
#   tools/host_stall_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
SHM="/hoststall_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }
[ -d "/Library/Audio/Plug-Ins/VST3/Zebra2.vst3" ] || { echo "SKIP: Zebra2 not installed"; exit 0; }

TMP="$(mktemp -d)"
ENG=""
FROZEN=""
# ALWAYS RESUME THE FROZEN HOST. A SIGSTOPped process ignores SIGTERM, so a check that dies
# without SIGCONT leaves a stopped process holding a plugin and an audio connection forever.
cleanup() {
  [ -n "$FROZEN" ] && kill -CONT "$FROZEN" 2>/dev/null
  [ -n "$ENG" ] && stop_engine "$ENG"
  rm -rf "$TMP"
}
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

( cd "$BUILD" && DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$ROOT/presets/projects" \
    DAW_CAPTURE_WAV="$TMP/h.wav" DAW_CAPTURE_SECONDS=16 DAW_ENGINE_DEBUG_STALL=1 \
    ./daw_engine --run-seconds 20 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120 "UI: command thread started"
cli() { env DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }
cli do load maximal --force >/dev/null 2>&1 || true
wait_for_event "$TMP/eng.log" "project.load" 60 >/dev/null 2>&1 || true

# Every host must be up, or freezing "one of them" is freezing an arbitrary subset.
hosts_ready() { [ "$(grep -c 'host ready for track' "$TMP/eng.log" 2>/dev/null)" -ge 6 ]; }
wait_until 60 hosts_ready || fail "not all six hosts came up; freezing one proves nothing.
$(tail -6 "$TMP/eng.log" | sed 's/^/          /')"

cli do play --force >/dev/null 2>&1 || true
sleep 4   # a real duration: the capture needs audible material BEFORE the freeze to compare against

# FREEZE ONE HOST. Its socket path carries the track index, so this picks track 0 specifically
# rather than whichever host the OS happens to list first.
FROZEN="$(pgrep -f "juce_host_process.*_0\.sock" | head -1)"
[ -n "$FROZEN" ] || FROZEN="$(pgrep -f juce_host_process | head -1)"
[ -n "$FROZEN" ] || fail "found no host process to freeze, so this check cannot pose its question"
kill -STOP "$FROZEN" || fail "could not SIGSTOP the host process $FROZEN"
echo "  froze host pid $FROZEN"
# An anchor IN THE ENGINE LOG, so "after the freeze" is a position in the same stream the
# stalls are counted from rather than a wall-clock guess.
echo "froze-host-marker" >> "$TMP/eng.log"
sleep 7

kill -CONT "$FROZEN" 2>/dev/null; FROZEN=""
wait "$ENG" 2>/dev/null || true; ENG=""

# THE DISCRIMINATOR IS THE PRODUCER, NOT AN AUDIO WINDOW. A first version of this compared
# rms(2,5) against rms(7.5,11.5) — absolute offsets into a capture whose origin is when the DEVICE
# started, not when the freeze happened. It passed with the fix REVERTED, which is the only reason
# I know the windows were meaningless: a control that cannot fail is not a control, and this is
# the third time tonight a fixed time window has measured the wrong stretch of audio.
#
# What the fix actually changes is whether the producer stays gated. Frozen host + no fix = the
# `inFlight` stall repeats forever, because the minimum can never advance past the frozen host.
# Frozen host + fix = the host is dropped and production continues. That is a statement about the
# engine, made from the engine's own words, and it needs no clock alignment at all.
STALLS_AFTER=$(awk '/froze-host-marker/{seen=1} seen && /producer stall \(inFlight\)/{n++} END{print n+0}' "$TMP/eng.log")
echo "  inFlight stalls after the freeze: $STALLS_AFTER"
if [ "$STALLS_AFTER" -gt 3 ]; then
  echo
  fail "the producer stayed gated after one host froze ($STALLS_AFTER inFlight stalls).

        It gates on the MINIMUM completed block across every host, so a host that stops advancing
        holds the transport at its last block and NOTHING is rendered again — for every track, not
        just its own. Five other tracks had nothing wrong with them.

        If this ever goes red it is a REGRESSION: the property held when this was written. The
        rule to look at is daw::engine::completedMinimum in apps/engine_rt_helpers.h and its
        caller's per-host collection loop."
fi

python3 - "$TMP/h.wav" <<'PY' || exit 1
import sys, wave, numpy as np
w = wave.open(sys.argv[1], 'rb'); sr = w.getframerate(); ch = w.getnchannels()
d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)/32768.0
if ch > 1: d = d.reshape(-1, ch).mean(axis=1)
def rms(a, b):
    s = d[int(a*sr):int(b*sr)]
    return float(np.sqrt((s**2).mean())) if s.size else 0.0
before, after = rms(2.0, 5.0), rms(7.5, 11.5)
print(f"  rms before the freeze={before:.5f}  after={after:.5f}  (context, not the verdict)")
# THE PRECONDITION IS "THE FIXTURE MADE SOUND", NOT "IT MADE SOUND IN SECONDS 2 TO 5".
#
# This used to fail the whole check when rms(2,5) was zero, which is an ABSOLUTE offset into a
# capture whose origin is when the DEVICE started — so on a loaded machine the device starts
# later, the window slides off the front of the audio, and the check reports "nothing was
# sounding" about a run that sounded perfectly well. Observed exactly that: before=0.00000,
# after=0.07274, stalls after the freeze 0. The verdict (the stall count) had already passed.
#
# The windows stay printed because they are useful context. What gates is whether the capture
# contains audio ANYWHERE, which is the thing the setup actually has to guarantee and is
# immune to where the origin fell.
peak = float(np.abs(d).max())
if peak < 1e-4:
    print(f"  FAIL(setup): the capture is silent end to end (peak {peak:.6f}), so the freeze")
    print("        was applied to a run that never made a sound and proves nothing.")
    raise SystemExit(1)
if False:  # the window numbers are CONTEXT, not the verdict — see the note above the awk.
    print("  FAIL: ONE FROZEN PLUGIN HOST SILENCED THE WHOLE SONG.")
    print("")
    print("        The producer gates on the minimum completed block across every host, so a host")
    print("        that stops advancing holds the transport at its last block and nothing is ever")
    print("        rendered again — for every track, not just its own. Five other tracks had")
    print("        nothing wrong with them.")
    print("")
    print("        The fix is in apps/engine_producer_thread.cpp: past a grace period a host that")
    print("        has not advanced stops counting toward that minimum (active=false, which")
    print("        completedMinimum honours first), so the song keeps playing without it.")
    raise SystemExit(1)
print("  the song kept playing with a frozen host")
PY


echo "host_stall_check: PASS — a frozen plugin host does not halt the transport" \
     "(a property that holds today, NOT proof the reported silence is fixed — see the header)"
