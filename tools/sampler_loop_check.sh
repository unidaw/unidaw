#!/usr/bin/env bash
# THE SAMPLE LOOP IS REACHABLE, AND TURNING IT ON IS AUDIBLE.
#
# SamplerSlotField covered twenty slot fields and MISSED seven that the voice actually renders:
# loopMode, sustainLoop, the three loop frame positions, and the two trim positions.
# sampler_voice.h reads loopMode and sustainLoop on every note, and docs/SAMPLER_DESIGN.md lists
# "sample loop modes (forward / ping-pong / backward) + seam-crossing interpolation + loop
# crossfade" as a HEADLINE of S3. All of it worked. None of it could be switched on except by
# hand-editing project JSON.
#
# That is the third instance of one shape found in a night — after modSet.filterType, which was
# READ at the kit publish site and written nowhere, and every patcher node config, whose command
# edited the wrong graph. The engine reads a field it has no path to write, and every structural
# fact around it is correct, so nothing downstream can notice.
#
# WHAT MAKES A LOOP MEASURABLE: a sample far SHORTER than the note. Without a loop the voice runs
# out of sample and goes quiet; with one it sustains for the whole note. So the property is
# "still sounding late in the note", which is what a loop IS, rather than a field read back.
#
# THREE PROPERTIES:
#   ONE-SHOT      loopMode 0 is silent in the tail — the control, and without it a fixture that
#                 sustained for any other reason would make the loop test meaningless
#   LOOPED        loopMode 1 set IN THE PROJECT sustains through the tail
#   COMMANDED     loopMode 1 set BY COMMAND does the same, which is the half that was missing
#
#   tools/sampler_loop_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

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
. "$ROOT/tools/lib/engine_wait.sh"

# A QUARTER-SECOND TONE, and the notes below are two seconds long. The gap is the whole
# measurement: without a loop there is nothing left to play after 0.25 s.
SAMPLE_FRAMES=12000
python3 - "$TMP/s.wav" "$SAMPLE_FRAMES" <<'PY'
import sys, wave, struct, math
sr = 48000
n = int(sys.argv[2])
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
# A STEADY tone, not a decaying one: a decay would make the loop's tail quiet for a reason that
# has nothing to do with looping, and the assertion below would be measuring the envelope.
w.writeframes(b''.join(
    struct.pack('<h', int(13000 * math.sin(2 * math.pi * 400.0 * i / sr))) for i in range(n)))
w.close()
PY

# project <name> <loopMode>
project() {
  python3 - "$TMP/$1.uniproj.json" "$Q" "$2" "$SAMPLE_FRAMES" <<'PY'
import json, sys
out, Q, loop_mode, frames = (sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        # THE LOOP SPANS THE WHOLE SAMPLE. loop_mode is the only thing that differs between the
        # two projects, so a difference in the tail can only be the loop.
        "loop_start_frame": 0, "loop_end_frame": frames, "loop_xfade_frames": 0,
        "loop_mode": loop_mode, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 0, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        # GATED, so the note lasts as long as the row says rather than one-shotting.
        "gate": 1, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 8, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "s.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
# ONE note, three quarters long — six times the sample, and deliberately SHORTER than the clip.
# A note whose duration exactly equals the clip length renders SILENT here: its note-off wraps to
# tick 0 and lands on the same tick as its note-on. That is its own finding and not this check's
# subject, so the fixture stays clear of it rather than measuring it by accident.
notes = [{"nanotick": 0, "duration": Q * 3, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": 1}]
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "l"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY
}

render() {  # render <project> <name>
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/loop_${$}_$2" \
      ./daw_engine --project "$1" --render "$2" --run-seconds 4 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

# TAIL ENERGY: the peak between 1.0 s and 1.5 s, which is well past the 0.25 s sample and well
# inside the 2 s note.
tail_peak() {
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(sr * 1.0), int(sr * 1.5)
seg = [abs(s[i * ch]) for i in range(a, min(b, n))]
print(max(seg) if seg else 0)
PY
}

# ---- ONE-SHOT. The control: no loop, so the tail is silent.
project noloop 0
render noloop noloop
OFFPEAK="$(tail_peak noloop)"
echo "  loop off: tail peak $OFFPEAK"
HEAD="$(python3 - "$TMP/noloop.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n = w.getnchannels(), w.getnframes()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
print(max(abs(s[i * ch]) for i in range(min(8000, n))))
PY
)"
[ "${HEAD:-0}" -gt 2000 ] || \
  fail "the unlooped render is silent even at the START (peak ${HEAD:-0}), so the fixture never
        played at all and a silent tail below would prove nothing about looping"
[ "${OFFPEAK:-0}" -lt 500 ] || \
  fail "the unlooped render is still sounding at 1.0-1.5 s (peak $OFFPEAK) although its sample is
        only 0.25 s long. Something else is sustaining it, so the loop test underneath cannot
        attribute a loud tail to the loop"

# ---- LOOPED, set in the project.
project loop 1
render loop loop
ONPEAK="$(tail_peak loop)"
echo "  loop on:  tail peak $ONPEAK"
[ "${ONPEAK:-0}" -gt 2000 ] || \
  fail "loop_mode 1 in the project did not sustain: tail peak ${ONPEAK:-0} against $OFFPEAK
        unlooped. The voice reads loopMode on every note, so a silent tail means the field is not
        reaching it"

# ---- COMMANDED. The half that was missing: loopMode had no field id, so no command could set it.
if [ -x "$CLI" ]; then
  ( cd "$BUILD" && exec env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/loopcmd_$$" \
      ./daw_engine --project noloop --run-seconds 20 >"$TMP/cmd.log" 2>&1 ) &
  ENG=$!
  wait_for_boot "$TMP/cmd.log" "$ENG" 160
  grep -q '"event":"project.load"' "$TMP/cmd.log" 2>/dev/null || \
    fail "the engine never loaded — see $TMP/cmd.log"
  cli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/loopcmd_$$" "$CLI" "$@"; }
  cli do sampler-slot --track 0 --device 1 --slot 1 --field loop-mode --value 1 >/dev/null 2>&1
  cli do sampler-slot --track 0 --device 1 --slot 1 --field loop-end --value "$SAMPLE_FRAMES" \
    >/dev/null 2>&1
  sleep 1.0
  cli do save cmdloop >/dev/null 2>&1
  sleep 1.5
  kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
  [ -f "$TMP/cmdloop.uniproj.json" ] || fail "the engine did not save cmdloop — see $TMP/cmd.log"
  python3 - "$TMP/cmdloop.uniproj.json" <<'PYC' || fail "sampler-slot --field loop-mode did not
        reach the slot. loopMode had no entry in SamplerSlotField at all, so the command could
        not name it — the field was rendered on every note and settable by nothing"
import json, sys
d = json.load(open(sys.argv[1]))
for t in d["tracks"]:
    for dev in t["device_chain"]:
        for s in dev.get("sampler", {}).get("slots", []):
            if s.get("loop_mode") == 1:
                raise SystemExit(0)
            print("  loop_mode is %r, expected 1" % s.get("loop_mode"))
raise SystemExit(1)
PYC
  # AND IT MUST SOUND, not merely persist. A field that saves and does not reach the voice is
  # the same defect one layer along, which is exactly what filterType turned out to be.
  render cmdloop cmdloop
  CMDPEAK="$(tail_peak cmdloop)"
  echo "  loop set by command: tail peak $CMDPEAK"
  [ "${CMDPEAK:-0}" -gt 2000 ] || \
    fail "loop_mode set BY COMMAND saved correctly and does not sustain (tail peak
        ${CMDPEAK:-0}). It persisted and did not reach the voice"
else
  echo "  note: daw-cli not built — the command path was not checked"
fi

echo "sampler_loop_check: PASS — the loop is reachable from a command and audible when it is"
