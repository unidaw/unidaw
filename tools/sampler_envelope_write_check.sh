#!/usr/bin/env bash
# THE ADSR WAS UNREACHABLE.
#
# `sampler-slot` could CHOOSE which mod set a slot uses and nothing could edit what was IN one, so
# the single most-used control on any sampler — how the sound fades in and out — could only be
# set by hand-editing the project JSON. SamplerSetEnvelope (opcode 82) is the command that was
# missing.
#
# ASSERTED ON THE AUDIO, not on the document. A check that read the envelope back out of the
# project file would prove the command stored some numbers; it would pass just as happily with
# the envelope never reaching a voice, which is the failure that matters. So this renders the
# same note three ways and listens to the shape.
#
# FOUR PROPERTIES:
#   ATTACK    a long attack makes the note START QUIET and arrive later. Measured as the ratio of
#             early energy to late energy, which is the shape rather than the level
#   SUSTAIN   a low sustain holds the note QUIETER after the decay than a full one does
#   RELEASE   a long release makes the sound OUTLAST its note-off
#   MINTS     it works on a mod set with NO modulators at all — the state every project starts
#             in. If it required an existing envelope, the common case would need a command that
#             does not exist
#
# Needs a real audio device (non-test mode) + daw_engine and daw-cli built: the command is sent
# to a LIVE engine, then the project is saved and rendered offline, so what is measured is the
# audio produced by the state the command left behind.
#   tools/sampler_envelope_write_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# A steady two-second tone. Steady on purpose: any change in the rendered ENVELOPE is then the
# envelope's doing and not the sample's, so the measurement needs no reference to what the source
# was doing at that instant.
python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
w = wave.open(os.path.join(sys.argv[1], "tone.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(16000 * math.sin(2 * math.pi * 220.0 * i / sr)))
                       for i in range(sr * 2)))
w.close()
PY

# ONE note, held for a beat, with a MOD SET THAT HAS NO MODULATORS. That is the MINTS property:
# every project starts here, and a command that could only edit an existing envelope would be
# useless until something else created one.
python3 - "$TMP/env.uniproj.json" "$TMP" "$Q" <<'PY'
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
        # GATED, not one-shot. gate 0 ignores note-off entirely and plays the sample out, so
        # the "no envelope" tail measured exactly the same as the note itself and the release
        # assertion compared 5344 against 5344 — a comparison with nothing in it. A release is
        # only observable against a voice that would otherwise have STOPPED.
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
# One note at 0.5 s, held for a beat (0.5 s), so note-off is at 1.0 s and the release tail is
# measurable in the quiet after it.
notes = [{"nanotick": Q, "duration": Q, "pitch": 60, "velocity": 120,
          "column": 0, "note_id": 1}]
clip = {"id": 1, "name": "p", "length": BAR, "kind": "symbolic", "notes": notes}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "env"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

start_engine() {  # $1=shm  $2=logfile
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="$1" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --run-seconds 30 >"$2" 2>&1 ) &
  ENG=$!
  for _ in $(seq 1 160); do
    if grep -q 'starting threads' "$2" 2>/dev/null; then return 0; fi
    sleep 0.25
  done
  fail "the engine never came up (see $2)"
}

# shape <name> <cli args...> — send an envelope, save it under <name>, render it offline.
shape() {
  local name="$1"; shift
  local shm="/envchk_$$_$name"
  start_engine "$shm" "$TMP/$name.eng.log"
  DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" "$CLI" do load env --force >/dev/null 2>&1 || true
  sleep 1.2
  if [ "$#" -gt 0 ]; then
    DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" "$CLI" do sampler-env "$@" \
      >/dev/null 2>&1 || fail "sampler-env was refused for '$name'"
    sleep 0.6
  fi
  DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" "$CLI" do save "$name" --force >/dev/null 2>&1 || true
  sleep 1.5
  kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
  [ -f "$TMP/$name.uniproj.json" ] || fail "'$name' produced no saved project"
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/envrnd_$$_$name" \
      ./daw_engine --project "$name" --render "$name" --run-seconds 6 --block-size 256 \
      >"$TMP/$name.render.log" 2>&1 ) || fail "the '$name' render exited non-zero"
  [ -s "$TMP/$name.wav" ] || fail "the '$name' render wrote no output"
}

rms() {  # rms <wav> <startSec> <endSec>
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

# ---- The reference: no envelope command at all.
shape flat
F_EARLY="$(rms "$TMP/flat.wav" 0.52 0.60)"
F_LATE="$(rms "$TMP/flat.wav" 0.85 0.95)"
F_TAIL="$(rms "$TMP/flat.wav" 1.10 1.30)"
echo "  no envelope:  early=$F_EARLY late=$F_LATE tail=$F_TAIL"
[ "$F_EARLY" -gt 200 ] || fail "the note is inaudible with no envelope (early rms $F_EARLY) — the
        fixture is wrong, and every comparison below would be comparing silences"

# ---- ATTACK. 300 ms attack, full sustain: quiet at the start, arrived by 0.85 s.
shape attack --track 0 --amp --attack 300000 --decay 0 --sustain 1000 --release 0
A_EARLY="$(rms "$TMP/attack.wav" 0.52 0.60)"
A_LATE="$(rms "$TMP/attack.wav" 0.85 0.95)"
echo "  300ms attack: early=$A_EARLY late=$A_LATE"
python3 -c "
flat = $F_EARLY / max(1.0, $F_LATE)
att  = $A_EARLY / max(1.0, $A_LATE)
raise SystemExit(0 if att < flat * 0.5 else 1)" || \
  fail "a 300 ms attack did not change the note's SHAPE. early/late was
        $F_EARLY/$F_LATE with no envelope and $A_EARLY/$A_LATE with the attack — the ratio had to
        at least halve. Either the command stored nothing or what it stored never reached a voice"

# ---- SUSTAIN. Fast attack and decay to a low sustain: quieter after the decay than full.
shape sustain --track 0 --amp --attack 1000 --decay 50000 --sustain 200 --release 0
S_LATE="$(rms "$TMP/sustain.wav" 0.85 0.95)"
echo "  0.2 sustain:  late=$S_LATE  (vs $F_LATE with no envelope)"
python3 -c "
raise SystemExit(0 if $S_LATE < $F_LATE * 0.6 else 1)" || \
  fail "a sustain of 0.2 held the note at $S_LATE after its decay, against $F_LATE with no
        envelope at all. The sustain level is not reaching the voice"

# ---- RELEASE. The note ends at 1.0 s; a 400 ms release must still be sounding after it.
shape release --track 0 --amp --attack 1000 --decay 0 --sustain 1000 --release 400000
R_TAIL="$(rms "$TMP/release.wav" 1.10 1.30)"
echo "  400ms releas: tail=$R_TAIL  (vs $F_TAIL with no envelope)"
python3 -c "
raise SystemExit(0 if $R_TAIL > max(50.0, $F_TAIL * 2.0) else 1)" || \
  fail "with a 400 ms release the sound after note-off measured $R_TAIL, against $F_TAIL with no
        envelope. A release that does not outlast its note-off is not a release"

echo "sampler_envelope_write_check: PASS — the ADSR is reachable, and it is audible in all three"
echo "                              stages, on a mod set that started with no modulators at all"
