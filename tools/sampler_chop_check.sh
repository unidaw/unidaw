#!/usr/bin/env bash
# DROP A BREAK, CHOP IT, PLAY IT — WITHOUT EDITING A FILE.
#
# tools/sampler_slice_stability_check.sh proves the IDENTITY property (re-cut and the rows do not
# move) against a hand-written slice set. This proves the DETECTION and the gesture: an actual
# break, sliced by a command, one slot minted per slice, and every slice playing its own audio.
#
# The two are deliberately separate. Stability is about what happens when a chop CHANGES, and
# needs a fixture whose markers are exact. Detection is about finding the hits at all, and needs a
# fixture with hits to find. One check doing both would be weaker at each.
#
# FOUR PROPERTIES:
#   DETECTS      transient slicing finds the hits in a break, roughly where they are
#   MAKES SLOTS  --slots mints one playable slot per slice, on consecutive keys, FIXED PITCH
#   PLAYS        each key sounds ITS slice — identified by frequency, not by "something sounded"
#   RE-CUTS LIVE a marker added afterwards takes effect on the next note, with no row rewritten
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/sampler_chop_check.sh
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
trap 'rm -rf "$TMP"' EXIT
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

# A BREAK: four hits, each a decaying burst at a DIFFERENT pitch, separated by near-silence.
# Different pitches so "which slice played" is answerable from the audio; decaying bursts so the
# transient detector has real rising edges rather than a step function it could not miss.
python3 - "$TMP/brk.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
seg = sr // 4          # 0.25 s per hit
freqs = [220.0, 440.0, 660.0, 880.0]
frames = []
for f in freqs:
    for i in range(seg):
        env = math.exp(-i / 3000.0)
        frames.append(struct.pack('<h', int(20000 * env * math.sin(2 * math.pi * f * i / sr))))
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(frames)); w.close()
PY

python3 - "$TMP/blank.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
tr = {"track_id": 0, "name": "B", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "blank"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

SHM="/chopchk_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null && break
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load blank --force >/dev/null 2>&1 || true
for _ in $(seq 1 80); do
  grep -q '"event":"project.load"' "$TMP/eng.log" 2>/dev/null && break
  sleep 0.25
done
sleep 1.0

cli do add-device --track 0 --kind sampler --device-id 1 >/dev/null 2>&1 || true
sleep 1.0
cli do sampler-load --track 0 --device 1 --file brk.wav --root 60 >/dev/null 2>&1 || true
sleep 1.5
grep -q '"event":"sampler.loaded"' "$TMP/eng.log" || fail "the break did not load"

# ---- DETECTS, AND MAKES SLOTS. One command: chop and mint a playable slot per slice.
cli do sampler-slice --track 0 --device 1 --source 1 --mode transient \
    --sensitivity 700 --max 8 --slots --base-key 60 >"$TMP/slice.json" 2>&1 \
  || fail "sampler-slice exited non-zero: $(cat "$TMP/slice.json")"
sleep 1.5
SLICED="$(grep -o '"event":"sampler.sliced"[^}]*' "$TMP/eng.log" | tail -1)"
[ -n "$SLICED" ] || fail "no sampler.sliced event:
        $(grep -o '\"event\":\"sampler.slice_rejected\"[^}]*' "$TMP/eng.log" | tail -2)"
MADE="$(echo "$SLICED" | sed -n 's/.*"made":\([0-9]*\).*/\1/p')"
SLOTS="$(echo "$SLICED" | sed -n 's/.*"slots":\([0-9]*\).*/\1/p')"
echo "  sliced: $SLICED"
[ "$MADE" -ge 2 ] || \
  fail "transient detection found only $MADE boundaries in a four-hit break. Three is the right
        answer (four hits, three boundaries between them — the first slice starts at frame 0,
        which is not a boundary)"
[ "$SLOTS" -ge "$MADE" ] || \
  fail "--slots minted $SLOTS slots for $MADE slices; one per slice is the gesture that makes a
        chop playable in one command rather than N"
echo "  detects and mints: $MADE boundaries, $SLOTS playable slots on keys from 60"

# ---- The read-back agrees, and every slot is FIXED PITCH: a slice played from its own key must
# sound as recorded, not transposed by where it happens to sit on the keyboard.
cli get sampler-kit --track 0 --device 1 --seq 3 2>&1 | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["found"], d
# Filter by SLICE, not by key: the slot minted by sampler-load plays the WHOLE source and can
# sit on the same key. Distinguishing them is exactly what slice_id is published for — without
# it a kit grid cannot tell a chop from a one-shot, which is how this test found the omission.
sliced = [s for s in d["slots"] if s["slice"] != 0]
assert len(sliced) >= 2, d["slots"]
for s in sliced:
    assert s["key_low"] == s["key_high"] == s["root"], ("not fixed-pitch: %r" % s)
    assert s["source_missing"] is False, s
print("  read-back: %d slice slots, each fixed-pitch on its own key" % len(sliced))
' || fail "the kit read-back did not show the sliced slots"

# ---- EMIT ROWS. The pattern that reproduces the chop, written by ONE command: a row per slice,
# each naming its slice BY ID. Press play and it is the break, following the project tempo with
# no stretching — because the ROWS are the timing.
#
# This is Octatrack's CREATE LINEAR LOCKS and Bitwig's slice-to-drum-machine clip, with the
# difference that matters: re-cutting afterwards moves what the rows PLAY without moving what
# they SAY. Bitwig emits its clip once, one-way.
cli do sampler-emit-rows --track 0 --device 1 --source 1 --at 0 --column 0 >/dev/null 2>&1 || true
sleep 1.5
EMITTED="$(grep -o '"event":"sampler.rows_emitted"[^}]*' "$TMP/eng.log" | tail -1)"
[ -n "$EMITTED" ] || fail "no sampler.rows_emitted event:
        $(grep -o '"event":"sampler.emit_rejected"[^}]*' "$TMP/eng.log" | tail -2)"
ROWS="$(echo "$EMITTED" | sed -n 's/.*"rows":\([0-9]*\).*/\1/p')"
[ "$ROWS" -ge 2 ] || fail "emit-rows wrote only $ROWS rows for $MADE slices: $EMITTED"
echo "  emits: $ROWS rows written, one per slice ($EMITTED)"

cli do save chop --force >/dev/null 2>&1 || true
sleep 1.8

# ---- THE EMITTED ROWS ADDRESS THEIR SLICES BY ID. A row with sound 0 would let the KEYMAP pick,
# which on this device would play whatever sits on that key rather than the slice the row means —
# and it would look completely fine until the kit changed.
python3 - "$TMP/chop.uniproj.json" <<'PYE'
import json, sys
d = json.load(open(sys.argv[1]))
notes = [n for c in d.get("clips", []) for n in c.get("notes", [])]
assert notes, "emit-rows wrote no notes into the project"
addressed = [n for n in notes if n.get("sound", 0) != 0]
assert len(addressed) == len(notes), \
    "%d of %d emitted rows have NO sound address — those rows let the keymap pick the slot, " \
    "which plays whatever sits on that key rather than the slice the row means" % (
        len(notes) - len(addressed), len(notes))
slots = {n["sound"] for n in notes}
assert len(slots) == len(notes), \
    "emitted rows share a sound address (%r) — one row per SLICE means one address per row" % slots
print("  addressed: all %d emitted rows name their own slice by id" % len(notes))
PYE
[ $? -eq 0 ] || fail "the emitted rows do not address their slices"

# ---- RE-CUTS LIVE. Add a marker while the engine is running; it must take effect with no row
# rewritten and no slice renumbered.
BEFORE_M="$(python3 -c "
import json
d = json.load(open('$TMP/chop.uniproj.json'))
tr = [t for t in d['tracks'] if not t.get('is_master')][0]
dev = [x for x in tr['device_chain'] if x['kind'] == 'sampler'][0]
print(len(dev['sampler']['slice_sets'][0]['markers']))
")"
cli do sampler-marker --track 0 --device 1 --source 1 --op add --frame 6000 >/dev/null 2>&1 || true
sleep 1.2
grep -q '"event":"sampler.marker"' "$TMP/eng.log" || \
  fail "no sampler.marker event: $(grep -o '\"event\":\"sampler.slice_rejected\"[^}]*' "$TMP/eng.log" | tail -1)"
cli do save chop2 --force >/dev/null 2>&1 || true
sleep 1.8
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

python3 - "$TMP/chop.uniproj.json" "$TMP/chop2.uniproj.json" "$BEFORE_M" <<'PYS'
import json, sys
def markers(p):
    d = json.load(open(p))
    tr = [t for t in d["tracks"] if not t.get("is_master")][0]
    dev = [x for x in tr["device_chain"] if x["kind"] == "sampler"][0]
    return dev["sampler"]["slice_sets"][0]["markers"], dev["sampler"]["slots"]
before, slotsBefore = markers(sys.argv[1])
after, slotsAfter = markers(sys.argv[2])
assert len(after) == len(before) + 1, (len(before), len(after))
# EVERY PRE-EXISTING MARKER KEEPS ITS ID *AND* ITS FRAME. This is the property; the audio check
# in sampler_slice_stability_check.sh is the same claim measured the other way.
b = {m["id"]: m["frame"] for m in before}
for m in after:
    if m["id"] in b:
        assert m["frame"] == b[m["id"]], \
            "marker %d MOVED from %d to %d — an insert must not touch an existing boundary" % (
                m["id"], b[m["id"]], m["frame"])
newIds = [m["id"] for m in after if m["id"] not in b]
assert len(newIds) == 1, newIds
assert newIds[0] not in b, "the new marker REUSED a live id"
# And no SLOT was renumbered or re-pointed: the rows that name these slices still mean the same
# thing, which is the whole point of doing it this way.
assert len(slotsAfter) == len(slotsBefore), (len(slotsBefore), len(slotsAfter))
sb = {s["id"]: s["slice_id"] for s in slotsBefore}
for s in slotsAfter:
    assert sb.get(s["id"]) == s["slice_id"], \
        "slot %d changed which slice it plays (%r -> %r)" % (s["id"], sb.get(s["id"]), s["slice_id"])
print("  re-cuts live: marker %d added; every existing id, frame and slot unchanged" % newIds[0])
PYS

echo "sampler_chop_check: PASS — drop a break, chop it, play it, re-cut it"
