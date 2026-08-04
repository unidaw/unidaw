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
# EIGHT PROPERTIES:
#   ATTACK    a long attack makes the note START QUIET and arrive later. Measured as the ratio of
#             early energy to late energy, which is the shape rather than the level
#   SUSTAIN   a low sustain holds the note QUIETER after the decay than a full one does
#   RELEASE   a long release makes the sound OUTLAST its note-off
#   LFOs      ModKind::Lfo renders at all. A pitch LFO is vibrato, measured as the note's
#             PERIOD wandering — which amplitude, filter and pan modulation cannot fake
#   PANNING   a pan envelope MOVES the sound across the stereo field, which neither a
#             volume nor a filter envelope can imitate
#   READ-BACK the kit says what a slot's mod set DOES — one bit per (target, kind), plus
#             the filter type, and only for modulators that would actually move something
#   TARGETS   an envelope can drive CUTOFF, not only volume. Asserted on BRIGHTNESS (the
#             high-band share of the energy) rather than level, because a level change is
#             what a volume envelope on the wrong target would look like
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

# A steady two-second SAW. Steady on purpose: any change in the rendered ENVELOPE is then the
# envelope's doing and not the sample's, so the measurement needs no reference to what the source
# was doing at that instant.
#
# A SAW AND NOT A SINE, and this cost a failing run to notice. The TARGETS property below measures
# a filter opening, and a sine has ONE partial — a low-pass sweep over it changes its amplitude
# and nothing else, so "brightness" is not a thing a sine can express. The check duly reported
# that the cutoff envelope did nothing. It was right about the measurement and wrong about the
# cause: there was no timbre to move.
python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
w = wave.open(os.path.join(sys.argv[1], "tone.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
frames = []
for i in range(sr * 2):
    phase = (220.0 * i / sr) % 1.0
    frames.append(struct.pack('<h', int(11000 * (2.0 * phase - 1.0))))
w.writeframes(b''.join(frames))
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
           # FILTER ON, AND NEARLY CLOSED. filter_type 0 means OFF (sampler_voice.h:78), so the
           # first version of this fixture asked a switched-off filter to sweep. cutoff_milli is
           # logarithmic over the audible range: 400 is about 317 Hz, just above the saw's 220 Hz
           # fundamental, so the harmonics are gone until an envelope opens it.
           "mod_sets": [{"id": 1, "name": "d", "filter_type": 1, "cutoff_milli": 400,
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
  ( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$1" DAW_PROJECT_DIR="$TMP" \
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

# ---- TARGETS. The engine renders envelopes on Cutoff, Pitch and Panning too, and for a while
# nothing could create one — the same gap the ADSR itself had, one level in. A cutoff envelope
# with a slow attack opens the filter over time, so the note gets BRIGHTER: high-frequency energy
# arrives late even though the source is steady. Measured as the ratio of high-band to total,
# which is timbre rather than level — a gain change moves both and leaves the ratio alone.
#
# BOTH WINDOWS SIT INSIDE THE NOTE (which runs 0.5 s to 1.0 s). The first version measured the
# late window at 0.95-1.03, straddling note-off, so it was reading the release rather than the
# filter — and reported a brightness rise for a project with NO envelope at all.
shape cutoff --track 0 --amp --target cutoff --depth 1000 \
    --attack 400000 --decay 0 --sustain 1000 --release 0
BRIGHT_EARLY="$(python3 - "$TMP/cutoff.wav" 0.52 0.60 <<'PYB'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(float(sys.argv[2]) * sr), min(n, int(float(sys.argv[3]) * sr))
# First difference is a crude high-pass: it is the derivative, so it rises with brightness.
tot = hi = 0.0
prev = 0.0
for i in range(a, b):
    v = float(s[i * ch])
    tot += v * v
    hi += (v - prev) ** 2
    prev = v
print(int(1000 * math.sqrt(hi / max(1e-9, tot))))
PYB
)"
BRIGHT_LATE="$(python3 - "$TMP/cutoff.wav" 0.86 0.96 <<'PYB'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(float(sys.argv[2]) * sr), min(n, int(float(sys.argv[3]) * sr))
tot = hi = 0.0
prev = 0.0
for i in range(a, b):
    v = float(s[i * ch])
    tot += v * v
    hi += (v - prev) ** 2
    prev = v
print(int(1000 * math.sqrt(hi / max(1e-9, tot))))
PYB
)"
echo "  cutoff env:   brightness early=$BRIGHT_EARLY late=$BRIGHT_LATE"
python3 -c "
raise SystemExit(0 if $BRIGHT_LATE > $BRIGHT_EARLY * 1.3 else 1)" || \
  fail "a CUTOFF envelope with a 400 ms attack did not open the filter: brightness went
        $BRIGHT_EARLY -> $BRIGHT_LATE. The engine renders cutoff envelopes; if this does not
        move, the command created an envelope on the wrong target — most likely Volume, which
        is what it did before --target existed"

# ---- PANNING. The pan envelope was STARTED, RELEASED, and never evaluated: spec.panDepth was
# set and never read, so a Panning envelope was a modulator the document promised and the sound
# never had. A slow pan envelope moves the source across the stereo field, so the LEFT/RIGHT
# BALANCE changes across the note — which is a thing neither a volume nor a filter envelope can
# do, so the measurement cannot be satisfied by the wrong target.
shape panning --track 0 --amp --target pan --depth 1000 \
    --attack 400000 --decay 0 --sustain 1000 --release 0
balance() {  # balance <wav> <startSec> <endSec>  -> right share of the energy, in thousandths
  python3 - "$1" "$2" "$3" <<'PYP'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(float(sys.argv[2]) * sr), min(n, int(float(sys.argv[3]) * sr))
l = r = 0.0
for i in range(a, b):
    l += float(s[i * ch]) ** 2
    r += float(s[i * ch + (1 if ch > 1 else 0)]) ** 2
print(int(1000 * math.sqrt(r) / max(1e-9, math.sqrt(l) + math.sqrt(r))))
PYP
}
PAN_EARLY="$(balance "$TMP/panning.wav" 0.52 0.60)"
PAN_LATE="$(balance "$TMP/panning.wav" 0.86 0.96)"
FLAT_EARLY="$(balance "$TMP/flat.wav" 0.52 0.60)"
FLAT_LATE="$(balance "$TMP/flat.wav" 0.86 0.96)"
echo "  pan env:      right share $PAN_EARLY -> $PAN_LATE (no envelope: $FLAT_EARLY -> $FLAT_LATE)"
python3 -c "
moved = abs($PAN_LATE - $PAN_EARLY)
still = abs($FLAT_LATE - $FLAT_EARLY)
raise SystemExit(0 if moved > 100 and moved > still * 4 else 1)" || \
  fail "a PAN envelope did not move the sound across the stereo field: the right channel's share
        went $PAN_EARLY -> $PAN_LATE, against $FLAT_EARLY -> $FLAT_LATE with no envelope at all.
        The pan envelope used to be started, released, and never evaluated"

# ---- LFOs. ModKind::Lfo was in SamplerModulator and in the saved project from the day the
# sampler shipped, and nothing in the engine or the voice ever looked at it: a modulator kind
# that saved, loaded, round-tripped perfectly and made no sound.
#
# A PITCH LFO is vibrato, and vibrato is measurable as something no other modulator can fake:
# the note's period WANDERS. Measured by counting zero crossings in successive 50 ms windows — a
# steady tone gives the same count every time, a vibrato'd one does not. Amplitude, filter and
# pan modulation all leave the crossing count alone.
#
# 50 ms windows and a deep swing on purpose. The first version used 20 ms at 220 Hz, which is
# about nine crossings — too few for an integer count to resolve a modest wobble, so a vibrato
# that was plainly working measured a spread of 3 and failed. The measurement has to be able to
# SEE the thing before its threshold means anything.
lfoShape() {  # like shape(), but sends sampler-lfo
  local name="$1"; shift
  local shm="/lfochk_$$_$name"
  start_engine "$shm" "$TMP/$name.eng.log"
  DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" "$CLI" do load env --force >/dev/null 2>&1 || true
  sleep 1.2
  DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" "$CLI" do sampler-lfo "$@" \
    >/dev/null 2>&1 || fail "sampler-lfo was refused for '$name'"
  sleep 0.6
  DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" "$CLI" do save "$name" --force >/dev/null 2>&1 || true
  sleep 1.5
  kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/lforn_$$_$name" \
      ./daw_engine --project "$name" --render "$name" --run-seconds 6 --block-size 256 \
      >"$TMP/$name.render.log" 2>&1 ) || fail "the '$name' render exited non-zero"
  [ -s "$TMP/$name.wav" ] || fail "the '$name' render wrote no output"
}
wobble() {  # wobble <wav> — spread of the zero-crossing count across 20 ms windows, in the note
  python3 - "$1" <<'PYW'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
win = int(0.05 * sr)
counts = []
start = int(0.55 * sr)
while start + win < int(0.98 * sr):
    c = 0
    prev = s[start * ch]
    for i in range(start + 1, start + win):
        v = s[i * ch]
        if (prev < 0) != (v < 0):
            c += 1
        prev = v
    counts.append(c)
    start += win
if not counts:
    print(0); raise SystemExit(0)
print(max(counts) - min(counts))
PYW
}
lfoShape vibrato --track 0 --target pitch --hz 6 --depth 1 --amount 300
VIB="$(wobble "$TMP/vibrato.wav")"
STEADY="$(wobble "$TMP/flat.wav")"
echo "  pitch LFO:    crossing spread $VIB (steady tone: $STEADY)"
python3 -c "
raise SystemExit(0 if $VIB > max(4, $STEADY * 3) else 1)" || \
  fail "a PITCH LFO produced no vibrato: the zero-crossing count varied by $VIB across the note,
        against $STEADY for a steady tone. ModKind::Lfo has been in the file format from the
        start; if this does not move, nothing is rendering it"

# ---- THE READ-BACK SAYS WHAT THE MOD SET DOES. A slot publishes a modSetId and, until now,
# nothing to resolve it against: a card could say "mod set 1" and not what mod set 1 does. modMask
# answers it in ten bits — bit (target * 2 + kind), so bit0 is an amp envelope and bit7 a cutoff
# LFO — and filterType goes beside it because a cutoff envelope on a filter that is OFF is silent,
# and a surface drawing that control would be drawing something that does nothing.
#
# A BIT MEANS "WOULD MOVE SOMETHING", not "is stored". That distinction is the whole reason this
# field is worth having: pan envelopes, resonance envelopes and every LFO were stored, loaded and
# rendered by nothing for a long time, and a read-back that reported them as modulators would
# have made the surface agree with the bug.
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
if [ -x "$CLI" ]; then
  MSHM="/envmask_$$"
  start_engine "$MSHM" "$TMP/mask.eng.log"
  kit() { DAW_UI_SHM_NAME="$MSHM" DAW_PROJECT_DIR="$TMP" "$CLI" get sampler-kit --track 0 --device 1 --seq "$1" 2>&1; }
  mcli() { DAW_UI_SHM_NAME="$MSHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
  mcli do load env --force >/dev/null 2>&1 || true
  sleep 1.2
  M0="$(kit 31 | grep -o '"mod_mask": [0-9]*' | head -1 | grep -o '[0-9]*')"
  FT="$(kit 32 | grep -o '"filter_type": [0-9]*' | head -1 | grep -o '[0-9]*')"
  mcli do sampler-env --track 0 --amp --target cutoff --depth 700 \
      --attack 100000 --decay 0 --sustain 1000 --release 0 >/dev/null 2>&1 || true
  sleep 0.6
  M1="$(kit 33 | grep -o '"mod_mask": [0-9]*' | head -1 | grep -o '[0-9]*')"
  mcli do sampler-lfo --track 0 --target pitch --hz 5 --depth 1 --amount 200 >/dev/null 2>&1 || true
  sleep 0.6
  M2="$(kit 34 | grep -o '"mod_mask": [0-9]*' | head -1 | grep -o '[0-9]*')"
  # AND AN INERT ONE MUST DROP OUT. Setting the LFO's swing to zero leaves the modulator stored
  # and round-tripping and doing nothing — which is exactly the state pan, resonance and every
  # LFO were in for months. If the mask still claimed it, a surface would draw a live control
  # over a dead one, and the read-back would be agreeing with the bug rather than exposing it.
  mcli do sampler-lfo --track 0 --target pitch --hz 5 --depth 0 --amount 200 >/dev/null 2>&1 || true
  sleep 0.6
  M3="$(kit 35 | grep -o '"mod_mask": [0-9]*' | head -1 | grep -o '[0-9]*')"
  kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
  echo "  mod mask:     $M0 (none) -> $M1 (+cutoff env) -> $M2 (+pitch LFO) -> $M3 (LFO zeroed), filter type $FT"
  # The fixture's mod set starts with NO modulators, so an empty mask is the honest answer.
  [ "${M0:-x}" = "0" ] || \
    fail "a mod set with no modulators reported mod_mask $M0. An inert or absent modulator must
          not be published as one — that is how a surface ends up drawing a control that does
          nothing"
  # bit (3*2+0) = 64 is a cutoff ENVELOPE.
  python3 -c "
raise SystemExit(0 if ($M1 & 64) != 0 else 1)" || \
    fail "after adding a cutoff envelope the mask was $M1, without bit 64 (target 3, kind 0) set"
  # bit (2*2+1) = 32 is a pitch LFO, and the cutoff envelope must still be there.
  python3 -c "
raise SystemExit(0 if ($M2 & 32) != 0 and ($M2 & 64) != 0 else 1)" || \
    fail "after adding a pitch LFO the mask was $M2 — expected both bit 32 (pitch LFO) and
          bit 64 (the cutoff envelope that was already there). A mask that loses what it had is
          worse than no mask"
  python3 -c "
raise SystemExit(0 if ($M3 & 32) == 0 and ($M3 & 64) != 0 else 1)" || \
    fail "an LFO with ZERO swing is still reported in the mask ($M3): bit 32 should have cleared
          and bit 64 stayed. A stored modulator that cannot move anything is not a modulator, and
          publishing it as one makes the surface agree with the bug instead of showing it"
  [ "${FT:-0}" = "1" ] || \
    fail "the read-back reported filter type ${FT} for a mod set configured as LP12 (1). A cutoff
          envelope means nothing without knowing whether the filter is even on"
fi

echo "sampler_envelope_write_check: PASS — the ADSR is reachable, and it is audible in all three"
echo "                              stages, on a mod set that started with no modulators at all"
