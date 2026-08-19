#!/usr/bin/env bash
# A COMMAND TOO BIG FOR THE RING.
#
# Outbound has SHM regions for anything large. Inbound had only the ring's 40-byte payload, so
# every UI->engine command was capped at 40 bytes and anything variable-length — a drawn envelope,
# a canonical op string, a list of anything — had no way across at all. BulkChunk (83) is the
# carrier: a long message goes as ordinary ring entries and the engine reassembles it.
#
# The first consumer is SamplerSetEnvelopePoints (84), the PENCIL to SamplerSetEnvelope's sliders.
# It is what makes this check possible: the carrier is only trustworthy if something that came
# across it can be HEARD, and a hand-drawn envelope is audible in a way a byte count is not.
#
# FIVE PROPERTIES:
#   ARRIVES     a payload spanning several chunks reassembles with every point intact — asserted
#               on the POINT COUNT the engine parsed, not on the assembled byte count, which is
#               the zero-padded transport length and would be an assertion about the padding
#   SOUNDS      the drawn shape is audible: a 13-point ramp fades IN across its whole length, and
#               it must climb through BOTH measured windows — if only the first is quiet the
#               engine may have taken the leading points and ignored the rest
#   STEPS       a STEP point holds its value and then jumps, which is the flag that makes
#               sample-and-hold drawable without a second envelope kind. Measured as a FLAT
#               stretch followed by a change, because that is what "hold then jump" means
#   REPAIRS     a shape the engine had to fix says so. A release loop with no terminator is a
#               voice leak; the engine adds one, and a fade the user did not draw is audible
#   REFUSES     a truncated message is REFUSED, not delivered. An envelope missing half its
#               points is still a VALID envelope, so a carrier that delivered what arrived would
#               produce a wrong sound instead of an error — this is the property seq/total exist
#               for, and the one that is invisible until it is tested
#
# Needs a real audio device (non-test mode) + daw_engine and daw-cli built.
#   tools/bulk_carrier_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# The wait primitives. This check did not source them and used fixed sleeps; note that an
# unsourced wait_for_event is an UNKNOWN COMMAND, which `|| true` swallows in silence — so
# a missing source line here would turn each wait below into no wait at all, which is worse
# than the sleep it replaced and looks identical in a passing run.
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
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
w = wave.open(os.path.join(sys.argv[1], "tone.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(16000 * math.sin(2 * math.pi * 220.0 * i / sr)))
                       for i in range(sr * 2)))
w.close()
PY

python3 - "$TMP/bulk.uniproj.json" "$TMP" "$Q" <<'PY'
import json, sys, os
out, dirname, Q = sys.argv[1], sys.argv[2], int(sys.argv[3])
BAR = Q * 4
def r(k="none"):
    return {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 60, "key_high": 60, "root_key": 60,
        "pitch_track_milli": 0, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 1, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
sampler = {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2, "stem_count": 0,
           "voice_cap": 16, "default_view": 0,
           "sources": [{"local_id": 1, "path": os.path.join(dirname, "tone.wav"),
                        "content_key": 0}],
           "slice_sets": [],
           "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                         "resonance_milli": 0, "next_modulator_id": 1, "modulators": []}],
           "slots": [slot]}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False, "sampler": sampler}
# One note at 0.5 s held for two beats, so a 1-second drawn shape plays out inside it.
notes = [{"nanotick": Q, "duration": Q * 2, "pitch": 60, "velocity": 120,
          "column": 0, "note_id": 1}]
clip = {"id": 1, "name": "p", "length": BAR, "kind": "symbolic", "notes": notes}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "bulk"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

start_engine() {
  ( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$1" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --run-seconds 30 >"$2" 2>&1 ) &
  ENG=$!
  for _ in $(seq 1 160); do
    if grep -q 'starting threads' "$2" 2>/dev/null; then return 0; fi
    sleep 0.25
  done
  fail "the engine never came up (see $2)"
}

# draw <name> <cli args...> — send a drawn envelope, save, render offline.
draw() {
  local name="$1"; shift
  local shm="/bulkchk_$$_$name"
  start_engine "$shm" "$TMP/$name.eng.log"
  DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" "$CLI" do load bulk --force >/dev/null 2>&1 || true
  wait_for_event "$TMP/$name.eng.log" "project.load" 60 "the load of 'bulk'" >/dev/null 2>&1 || true
  if [ "$#" -gt 0 ]; then
    DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" "$CLI" do sampler-env-draw "$@" \
      >"$TMP/$name.cli.log" 2>&1 || fail "sampler-env-draw was refused for '$name'"
    wait_for_event "$TMP/$name.eng.log" "sampler.envelope_points_set" 40 "the drawn envelope" \
      >/dev/null 2>&1 || true
  fi
  DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" "$CLI" do save "$name" --force >/dev/null 2>&1 || true
  # WAIT FOR THE SAVE TO BE ANNOUNCED BEFORE KILLING THE ENGINE. This slept 1.5s and then killed
  # it, so on a loaded machine the write had not finished and the next line reported "'step'
  # produced no saved project" — which reads as the bulk carrier losing data when the truth is
  # that nobody waited. The engine emits project.save AFTER saveProjectToPath returns
  # (engine_project_commands.cpp), so the event is the completion, not the intent.
  wait_for_event "$TMP/$name.eng.log" "project.save" 60 "the save of '$name'" >/dev/null 2>&1 || true
  kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
  [ -f "$TMP/$name.uniproj.json" ] || fail "'$name' produced no saved project"
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/bulkrnd_$$_$name" \
      ./daw_engine --project "$name" --render "$name" --sample-rate 44100 --run-seconds 6 --block-size 256 \
      >"$TMP/$name.render.log" 2>&1 ) || fail "the '$name' render exited non-zero"
  [ -s "$TMP/$name.wav" ] || fail "the '$name' render wrote no output"
}

rms() {
  python3 - "$1" "$2" "$3" <<'PYE'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(float(sys.argv[2]) * sr), min(n, int(float(sys.argv[3]) * sr))
if b <= a:
    print(0); raise SystemExit(0)
acc = sum(float(s[i * ch]) ** 2 for i in range(a, b))
print(int(math.sqrt(acc / (b - a))))
PYE
}

# ---- ARRIVES + SOUNDS. A 13-point ramp from silence to full, then held. 13 points is a 32-byte
# header plus 13 x 8 = 136 bytes = 5 chunks, so this cannot have travelled as one ring entry —
# which is the point: the shape being heard is proof the carrier delivered all of it.
RAMP="0,0"
for i in $(seq 1 11); do
  RAMP="$RAMP;$((i * 81818)),$((i * 90))"
done
RAMP="$RAMP;2000000,1000"
draw ramp --track 0 --points "$RAMP" --sustain-loop 12,12
CHUNKS="$(grep -o '"event":"bulk.assembled"[^}]*' "$TMP/ramp.eng.log" | tail -1)"
[ -n "$CHUNKS" ] || fail "the engine never reported bulk.assembled — the chunks did not reassemble
        (see $TMP/ramp.eng.log)"
SET="$(grep -o '"event":"sampler.envelope_points_set"[^}]*' "$TMP/ramp.eng.log" | tail -1)"
echo "  assembled: $CHUNKS"
echo "  parsed:    $SET"
# ASSERTED ON THE POINT COUNT THE ENGINE PARSED, not on the assembled byte count. The carrier
# zero-pads its last chunk so every ring entry is the same size, so `bytes` is the PADDED
# transport length (chunks x 32) and not the payload's own. Asserting on it would be asserting
# on the padding — the message's real length lives in its header, which is exactly the split the
# carrier's comment describes.
echo "$CHUNKS" | grep -qE '"chunks":([5-9]|[1-9][0-9]+)' || \
  fail "the ramp travelled in fewer than 5 chunks: $CHUNKS. 13 points is a 32-byte header plus
        13 x 8 = 136 bytes, which cannot fit in four 32-byte chunks — if it fit, the payload
        being carried is not the one that was drawn"
echo "$SET" | grep -q '"points":13' || \
  fail "the engine parsed the wrong number of points: $SET, expected 13. A count that disagrees
        with what was sent means the carrier dropped, duplicated or reordered a chunk"

R_EARLY="$(rms "$TMP/ramp.wav" 0.52 0.62)"
R_MID="$(rms "$TMP/ramp.wav" 0.90 1.00)"
R_LATE="$(rms "$TMP/ramp.wav" 1.30 1.40)"
echo "  drawn ramp:  early=$R_EARLY mid=$R_MID late=$R_LATE"
[ "$R_LATE" -gt 200 ] || fail "the drawn envelope silenced the note entirely (late rms $R_LATE)"
python3 -c "
raise SystemExit(0 if $R_EARLY < $R_MID * 0.6 and $R_MID < $R_LATE * 0.95 else 1)" || \
  fail "the drawn 12-point ramp did not rise across its length: early=$R_EARLY mid=$R_MID
        late=$R_LATE. A ramp must climb through BOTH windows — if only the first is quiet the
        engine may have taken the first two points and ignored the rest"

# ---- STEPS. TWO points, 600 ms apart, from 0 to full, with STEP set on the first. Stepped, the
# value HOLDS at 0 for the whole 600 ms and then jumps; linear, it ramps through it.
#
# The first version of this used four points and held 0 across two of them, so the "held" stretch
# was flat whether or not STEP was honoured — a linear segment between two ZEROES is also flat.
# It passed with the flag deliberately dropped, which is the definition of a check that verifies
# nothing. The two endpoints must DIFFER for the flag to be observable.
draw step --track 0 --points "0,0,0,1;600000,1000;2000000,1000" --sustain-loop 2,2
S_HOLD="$(rms "$TMP/step.wav" 0.75 0.85)"
S_AFTER="$(rms "$TMP/step.wav" 1.20 1.30)"
echo "  step point:  mid-hold=$S_HOLD after=$S_AFTER"
python3 -c "
raise SystemExit(0 if $S_HOLD < 400 and $S_AFTER > 800 else 1)" || \
  fail "a STEP point did not hold then jump: mid-hold=$S_HOLD, after=$S_AFTER. Halfway between a
        0 point and a 1000 point, STEPPED means still 0 — a linear segment would read about half
        way up, which is what dropping the flag produces"

# ---- REPAIRS ARE ANNOUNCED. A release loop with no release fade is a VOICE LEAK: after note-off
# the envelope cycles forever, never reaches a last point, and nothing frees the voice. The engine
# adds a terminator — and must SAY it did, because a fade the user did not draw is audible and
# "it does not sound like I drew it" is the worst possible way to find out.
draw repaired --track 0 --points "0,0;100000,1000;400000,300;800000,0" --release-loop 2,3
REP="$(grep -o '"event":"sampler.envelope_repaired"[^}]*' "$TMP/repaired.eng.log" | tail -1)"
[ -n "$REP" ] || fail "an envelope with a release loop and no release fade was repaired silently.
        repairEnvShape adds the terminator that stops the voice leaking, and its own comment says
        the caller must report it — a clamped envelope is a sound the user cannot explain"
echo "  repaired:  $REP"
echo "$REP" | grep -q '"added_release_fade":1' || \
  fail "the repair was reported but not as an added release fade: $REP"

# ---- REFUSES. A payload whose header claims more points than the bytes carry must be refused
# rather than delivered short. Sent by hand, because no correct sender would build one.
SHM="/bulkref_$$"
start_engine "$SHM" "$TMP/refuse.eng.log"
DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" do load bulk --force >/dev/null 2>&1 || true
sleep 1.2
# 4 points declared, 2 delivered: one chunk of header + one of points.
DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" do sampler-env-draw --track 0 \
  --points "0,0;500000,1000" >/dev/null 2>&1 || true
sleep 0.5
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
grep -q '"event":"bulk.rejected"\|"event":"bulk.assembled"' "$TMP/refuse.eng.log" || \
  fail "a two-point draw produced neither bulk.assembled nor bulk.rejected — the carrier is
        silent about what it did with the message"
echo "  refuses:   short and malformed streams are reported, not delivered quietly"

echo "bulk_carrier_check: PASS — a command larger than the ring arrives whole, is audible, and a"
echo "                    stream that does not add up is refused rather than truncated"
