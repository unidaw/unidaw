#!/usr/bin/env bash
# THE MASTER STRIP IS A TRACK LIKE THE OTHERS — IT MOVES THE MIXER VERSION, AND ITS METER MOVES.
#
# Two gaps reported by the web-UI agent against PR #19, both of the same family as the ones this
# repo keeps finding: a value that is published and CORRECT, and a signal that never says it
# changed, so nothing downstream ever re-reads it.
#
#   uiMixerVersion   `mixerChanged` compared each TRACK slot against lastGainMillibels[], and the
#                    master's slot was filled in an append block AFTER that comparison. So a
#                    master-only fader move published the new gain perfectly and left the version
#                    word untouched: an optimistic UI strip stays pending for ever, and the edit
#                    itself was never the problem.
#   uiTrackPeakRms   the master's entry was the literal `0.0f`, with a comment deferring it. An
#                    empty master meter is not a statement about the mix, it is the absence of
#                    one, and nothing could tell those apart.
#
# THE VERSION WORD WAS UNREADABLE FROM EVERY SURFACE until this change, which is why the first
# defect was not merely uncaught but unobservable: `get tracks` now prints mixer_version.
#
# FOUR PROPERTIES:
#   READABLE     mixer_version is published and non-zero
#   MOVES        a MASTER-only gain edit advances it — the reported defect, asserted first
#   PER EDIT     it does NOT advance when nothing changed, or "moves" is satisfied by a counter
#                that simply increments every frame, which would pass the assertion above while
#                telling a UI to re-read continuously
#   TRACK TOO    an ordinary track's edit still advances it, so the fix did not trade one slot
#                for another
#
# The MASTER METER (uiTrackPeakRms[master]) needs real audio and therefore a working output
# device. This machine has none — see daw_audio_probe — so that property SKIPS with its reason
# rather than failing, and the skip is gated on the probe, which shares nothing with the code
# under test beyond the device layer itself.
#
# No audio device needed for the four properties above: a version word is arithmetic.
#   tools/master_mixer_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

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

# TWO TRACKS, so "the master moved it" and "any edit moves it" are distinguishable, and so the
# last property has an ordinary track to edit that is not the one being watched.
python3 - "$TMP/mm.uniproj.json" <<'PY'
import json, sys
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def track(i):
    return {"track_id": i, "name": "T%d" % i, "harmony_quantize": False, "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                        "audio_out": r("master"), "pre_fader_send": True},
            "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "mm"}, "nanoticks_per_quarter": 960000,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [track(0), track(1)]}, open(sys.argv[1], "w"))
PY

SHM="/mmchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project mm --run-seconds 60 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

mver() {
  cli get tracks 2>/dev/null | python3 -c "
import re, sys
m = re.search(r'\"mixer_version\": (\d+)', sys.stdin.read())
print(m.group(1) if m else 'missing')
" 2>/dev/null
}
mastergain() {
  cli get tracks 2>/dev/null | python3 -c "
import re, sys
for line in sys.stdin:
    if '\"master\": true' in line:
        m = re.search(r'\"gain_db\": (-?[\d.]+)', line)
        print(m.group(1) if m else 'nogain'); raise SystemExit
print('nomaster')
" 2>/dev/null
}
masterpeak() {
  cli get tracks 2>/dev/null | python3 -c "
import re, sys
for line in sys.stdin:
    if '\"master\": true' in line:
        m = re.search(r'\"peak_rms\": (-?[\d.eE+]+)', line)
        print(m.group(1) if m else 'nopeak'); raise SystemExit
print('nomaster')
" 2>/dev/null
}
waitver() {  # waitver <not-equal-to>
  for _ in $(seq 1 60); do
    V="$(mver)"
    [ "$V" != "$1" ] && [ "$V" != "missing" ] && return 0
    sleep 0.25
  done
  return 1
}

# ---- READABLE.
V0="$(mver)"
[ "$V0" != "missing" ] || \
  fail "get tracks does not report mixer_version, so nothing below can be measured. Until this
        check that word was unreadable from every surface, which is why a master edit that never
        moved it was unobservable rather than merely uncaught"
[ "$V0" != "0" ] || fail "mixer_version is 0 after boot; the first publish should have moved it"
echo "  readable: mixer_version is $V0"

[ "$(mastergain)" = "0.00" ] || \
  fail "the master did not start at 0.00 dB, it reads '$(mastergain)' — the edit below would
        prove nothing"

# ---- PER EDIT, asserted BEFORE the change. If the version simply counts publishes, everything
# below passes while meaning nothing, so the still-life comes first.
#
# SETTLES FIRST, and this is not padding. The version legitimately moves a few times during boot
# as the track list, the master and the first mixer values arrive — measured going 1 -> 2 within
# the first second and then stopping. A bare `sleep` before sampling asserts that boot finishes
# inside that sleep, which is a claim about the machine; waiting for two consecutive equal reads
# asserts what is actually meant, which is that it has stopped moving.
SETTLED=""
for _ in $(seq 1 40); do
  A="$(mver)"; sleep 0.5; B="$(mver)"
  if [ "$A" = "$B" ] && [ "$A" != "missing" ]; then SETTLED="$A"; break; fi
done
[ -n "$SETTLED" ] || \
  fail "mixer_version never stopped moving over 20s with no edit at all. It is the signal a UI
        re-reads on, so a counter that free-runs tells every strip to re-read every frame — and
        it would make the 'moves' assertion below vacuous"
V0="$SETTLED"
sleep 1.5
V_STILL="$(mver)"
[ "$V_STILL" = "$V0" ] || \
  fail "mixer_version moved from $V0 to $V_STILL with no edit, after it had already settled"
echo "  per edit: settled at $V0 and did not move on its own over a further 1.5s"

# ---- MOVES, on a MASTER-ONLY edit. The reported defect.
cli do mixer --track master --gain-db -6 >/dev/null 2>&1
waitver "$V0" || \
  fail "a MASTER-only gain edit did not move mixer_version (still $V0). The edit itself lands —
        check the published gain, it will be right — but mixerChanged compares the per-TRACK
        slots and the master's slot was filled after that comparison, so nothing ever said the
        value moved and an optimistic strip stays pending for ever"
V1="$(mver)"
GOT="$(mastergain)"
[ "$GOT" = "-6.00" ] || \
  fail "the master's published gain reads '$GOT', wanted -6.00 — the version moved but the value
        did not, which is the opposite defect and worse"
echo "  moves: a master-only edit took mixer_version $V0 -> $V1, and the gain reads $GOT"

# ---- TRACK TOO. The fix must not have traded one slot for another.
cli do mixer --track 0 --gain-db -3 >/dev/null 2>&1
waitver "$V1" || \
  fail "an ordinary track's gain edit did not move mixer_version (still $V1) — the master fix
        broke the case that already worked"
echo "  track too: an ordinary track's edit moved it $V1 -> $(mver)"

# ---- THE MASTER METER, PROVEN WITHOUT AN AUDIO DEVICE.
#
# The first version of this block gated the meter on daw_audio_probe and SKIPPED on a machine
# that runs no callbacks — honest, and it meant the property went untested on the only machine
# anyone was running it on. It does not need a device: an OFFLINE RENDER drives the same
# callback through a pump, so the master bus is summed, its fader applied and its peak measured
# exactly as in a live run, while the publish loop keeps writing shared memory.
#
# AND IT IS COMPARED AGAINST THE RENDERED FILE, not merely asserted non-zero. "Some number
# appeared" would pass on a meter wired to the wrong buffer, to a track instead of the sum, or
# to a constant. The peak the engine PUBLISHED and the peak in the wav it WROTE are two
# independent paths to one quantity, and they have to agree.
PEAK="$(masterpeak)"
[ "$PEAK" != "nomaster" ] && [ "$PEAK" != "nopeak" ] || \
  fail "the master row has no peak_rms field at all, so the meter cannot be read"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

# FORTY BARS, so the render takes long enough to sample. A short one finishes faster than a
# single `get tracks` invocation, and a check that races its own subject is the flake shape this
# repo has spent the night removing.
python3 - "$TMP/tone.wav" "$TMP/long.uniproj.json" <<'PYL'
import json, sys, wave, struct, math
wav, out = sys.argv[1], sys.argv[2]
sr = 44100
w = wave.open(wav, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(20000 * math.sin(2 * math.pi * 440 * i / sr)))
                       for i in range(sr)))
w.close()
Q = 960000; BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
clips, places = [], []
for i in range(40):
    clips.append({"id": i + 1, "name": "a%d" % i, "length": BAR, "kind": "audio",
                  "audio": {"source_path": wav, "source_start_frame": 0, "gain_db": 0.0,
                            "fade_in": 0, "fade_out": 0}})
    places.append({"clip_id": i + 1, "id": i + 1, "at": i * BAR, "length": BAR,
                   "notes": [], "chords": [], "mutes": []})
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [], "placements": places}
json.dump({"schema_version": 4, "meta": {"name": "long"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": clips, "tracks": [tr]}, open(out, "w"))
PYL

SHM2="/mmchk2_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project long --render lout --sample-rate 44100 --run-seconds 120 >"$TMP/render.log" 2>&1 ) &
ENG=$!
cli2() { env DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
LIVEPEAK=""
for _ in $(seq 1 60); do
  P="$(cli2 get tracks 2>/dev/null | python3 -c "
import re, sys
for line in sys.stdin:
    if '\"master\": true' in line:
        m = re.search(r'\"peak_rms\": ([0-9.eE+-]+)', line)
        print(m.group(1) if m else ''); raise SystemExit
print('')
" 2>/dev/null)"
  case "${P:-0}" in
    ""|0|0.0|0.000000) sleep 0.25 ;;
    *) LIVEPEAK="$P"; break ;;
  esac
done
# LET THE RENDER FINISH rather than killing it the moment the peak is seen: the wav is finalised
# on a clean exit, so killing early leaves no file to compare against — which is exactly what the
# first version of this block did, and it reported "the render wrote nothing" about a render that
# was working perfectly.
wait "$ENG" 2>/dev/null; ENG=""

[ -n "$LIVEPEAK" ] || \
  fail "the master's published peak stayed at 0 through an entire offline render of forty bars
        of audio. uiTrackPeakRms[master] was the literal 0.0f with a comment deferring it, so an
        empty master meter said nothing about the mix — see $TMP/render.log"

[ -s "$TMP/lout.wav" ] || fail "the render wrote nothing, so there is no file to compare against"
python3 - "$TMP/lout.wav" "$LIVEPEAK" <<'PYC' || fail "the published master peak does not match
        what the engine actually rendered"
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
raw = w.readframes(w.getnframes())
s = struct.unpack('<%dh' % (len(raw) // 2), raw)
filepeak = (max(abs(x) for x in s) / 32768.0) if s else 0.0
live = float(sys.argv[2])
if filepeak <= 0.0:
    print("  the RENDER itself is silent (peak %.6f), so the comparison would be vacuous" % filepeak)
    raise SystemExit(1)
# Generous: the published value is one block's peak sampled at an arbitrary moment, the file's is
# the whole take's. They must be the same ORDER, which is what catches a meter wired to the wrong
# buffer, to a single track instead of the sum, or to a constant.
if not (filepeak / 4.0 <= live <= filepeak * 4.0):
    print("  published %.6f against a rendered peak of %.6f — more than a factor of four apart,"
          % (live, filepeak))
    print("  so the meter is not reading the master sum it claims to")
    raise SystemExit(1)
print("  meter: the master published %.6f while rendering, and the take peaks at %.6f"
      % (live, filepeak))
PYC

echo "master_mixer_check: PASS — the master moves the mixer version like any other track, and
                    its meter reports the sum it actually produced"
