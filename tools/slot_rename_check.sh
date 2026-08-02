#!/usr/bin/env bash
# A PAD HAS A NAME, YOU CAN READ IT, AND YOU CAN CHANGE IT.
#
# `SamplerSlot::name` was persisted by the project format from the day the sampler shipped and
# published by NOTHING. So it round-tripped through save and reload perfectly, no UI could read
# it, and no command could write it — the loader stamped the sample's FULL PATH on and that was
# the only value it ever held. Task #110.
#
# That is this codebase's most-repeated defect wearing its other face. Usually a persisted field
# cannot be WRITTEN (filterType, the loop frames, voiceCap, sourceLocalId, collapsed — eight
# times). This one could not be READ, which is why the sweep that found the other eight listed it
# and could not classify it: `name` is in no command's field enum because it is not an int32.
#
# FIVE PROPERTIES:
#   SEEDED BY LOAD    a loaded sample names its slot the file's STEM. Not the path — the path is
#                     what the SOURCE keeps, and a pad drawing /Users/.../BD_808.wav is the bug
#                     that publishing the name would otherwise have revealed
#   SEEDED BY SLICE   a chop names every slot it mints, and the names are DISTINCT. Slices were
#                     minted with no name at all, so a chopped kit published N empty strings.
#                     Distinctness is the half that matters: "slice" on all four would be
#                     non-empty and useless, and would pass any check that only tested emptiness
#   RENAMED           after opcode 90 the PUBLISHED name is exactly the string sent, and the
#                     other slot is untouched. Both halves: a rename that hits every slot is a
#                     different bug with the same read-back
#   PERSISTS          it survives save and reload. A rename you do again every session is not one
#   REFUSED           a name too long for the published field is rejected AND LEAVES THE SLOT
#                     ALONE. This is the property the web-UI agent asked for by name: a truncated
#                     name is worse than a refused one because it looks like it worked, and the
#                     file would then disagree with the screen forever with nothing reporting it
#
#   tools/slot_rename_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
# stop_engine, not a bare kill: it SIGTERMs, waits 10s, ESCALATES to SIGKILL and SAYS SO.
# A hung engine that ignores SIGTERM is left running by a bare kill, and ctest then waits
# about 1000s for it after timing the check out — measured across 18 runs, perfectly
# correlated with the timeout count. The escalation notice is also the diagnostic: "engine
# N ignored SIGTERM for 10s" names the hang instead of leaving it to be inferred.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# A file whose STEM is distinctive, IN A SUBDIRECTORY. The directory is the point: the loader
# used to stamp the whole path on, and a fixture that loaded from the project root would only
# ever prove the EXTENSION was dropped — a pad drawing "kits/BD_808.wav" would pass it.
mkdir -p "$TMP/kits"
python3 - "$TMP/kits/BD_808.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
# Four bursts, so an EQUAL chop into four is a musically sensible fixture as well as an arithmetic
# one. The content does not matter to this check; only that the file decodes and has length.
frames = []
for i in range(sr * 2):
    env = 1.0 if (i % (sr // 2)) < (sr // 20) else 0.0
    frames.append(struct.pack('<h', int(12000 * env * math.sin(2 * math.pi * 220 * i / sr))))
w.writeframes(b''.join(frames)); w.close()
PY

# ONE EMPTY SAMPLER DEVICE. Everything this check looks at is MINTED by a command, because the
# mint is half of what is being tested — a fixture that hand-wrote the slots would be blind to
# both seeding properties.
python3 - "$TMP/n.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 1, "next_source_id": 1, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 8, "default_view": 0,
                   "sources": [], "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": []}}
tr = {"track_id": 0, "name": "N", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "n"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

SHM="/slotname_$$"
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" \
    ./daw_engine --project n --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 40
ncli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }

# The PUBLISHED name of one slot, read out of the kit read-back — not out of the model and not
# out of the saved file. What a UI would draw.
pubname() {  # pubname <slotId>
  ncli get sampler-kit --track 0 --device 1 2>/dev/null | python3 -c "
import json, sys
try:
    k = json.load(sys.stdin)
except Exception:
    print('<no kit>'); raise SystemExit(0)
for s in k.get('slots', []):
    if s.get('slot') == int('$1'):
        print(s.get('name')); raise SystemExit(0)
print('<no slot $1>')"
}
slotcount() {
  ncli get sampler-kit --track 0 --device 1 2>/dev/null | python3 -c "
import json, sys
try: print(len(json.load(sys.stdin).get('slots', [])))
except Exception: print(0)"
}

# ---- SEEDED BY LOAD. The stem, not the path.
ncli do sampler-load --track 0 --device 1 --file kits/BD_808.wav --root 60 >/dev/null 2>&1
sleep 0.8
L1="$(pubname 1)"
echo "  loaded slot 1 publishes: '$L1'"
[ "$L1" = "BD_808" ] || \
  fail "a loaded slot publishes '$L1', expected 'BD_808'. An empty string means the name is not
        published at all — which is the state this whole check exists for. Anything containing a
        '/' means the loader is still stamping the full PATH on, which is what a pad would draw"

# ---- SEEDED BY SLICE, and distinct. Four equal slices, four named slots.
ncli do sampler-slice --track 0 --source 1 --mode equal --count 4 --slots >/dev/null 2>&1
sleep 1.0
N="$(slotcount)"
[ "$N" -ge 5 ] || fail "expected the load's slot plus 4 sliced ones, the kit has $N. See $TMP/eng.log"
S2="$(pubname 2)"; S3="$(pubname 3)"; S4="$(pubname 4)"; S5="$(pubname 5)"
echo "  sliced slots publish: '$S2' '$S3' '$S4' '$S5'"
for v in "$S2" "$S3" "$S4" "$S5"; do
  [ -n "$v" ] || \
    fail "a sliced slot publishes an EMPTY name. sampler-slice minted slots and set no name at
          all, so a chopped kit was N indistinguishable blanks"
done
[ "$S2" != "$S3" ] && [ "$S3" != "$S4" ] && [ "$S4" != "$S5" ] || \
  fail "two sliced slots publish the SAME name ('$S2' '$S3' '$S4' '$S5'). A constant is non-empty
        and useless — it cannot tell slice 1 from slice 4, which is the only thing the name is
        for here"
case "$S2" in
  BD_808*) : ;;
  *) fail "a sliced slot is named '$S2', which does not start with the source's stem. Two breaks
           chopped into one kit have to stay apart, and that is the normal case for a chop" ;;
esac

# ---- RENAMED. The published name is exactly what was sent, and only the slot named moves.
BEFORE3="$(pubname 3)"
ncli do sampler-slot-name --track 0 --device 1 --slot 2 --name "kick tail" >/dev/null 2>&1
sleep 0.8
grep -q '"event":"sampler.slot_renamed"' "$TMP/eng.log" 2>/dev/null || \
  fail "opcode 90 never reached the engine — no sampler.slot_renamed in $TMP/eng.log"
R2="$(pubname 2)"
echo "  after rename: slot 2 '$R2', slot 3 '$(pubname 3)'"
[ "$R2" = "kick tail" ] || \
  fail "slot 2 publishes '$R2' after being renamed to 'kick tail'. If it still reads the seeded
        name the command reached the model and not the RT snapshot the kit is published from —
        which is the exact trap opcode 88 fell into one field along"
[ "$(pubname 3)" = "$BEFORE3" ] || \
  fail "slot 3 was never named and moved from '$BEFORE3' to '$(pubname 3)'. The rename is hitting
        more than the slot it addresses"

# ---- REFUSED, not truncated. 40 bytes exactly: one past what the field can hold.
LONG="$(python3 -c "print('x' * 40)")"

# The CLI refuses it first, so a person gets told rather than getting a silent no-op.
#
# CAPTURED, NOT PIPED. daw-cli exits 2 when it refuses — correctly — and this script runs under
# `pipefail`, so `ncli ... | grep -q` returns 2 from the LEFT side of the pipe even when grep
# matched. That reads as "the guard is missing" while the guard is working perfectly, which is
# the worst kind of check failure: it accuses the code under test of the one thing it does right.
REFUSAL="$(ncli do sampler-slot-name --track 0 --device 1 --slot 3 --name "$LONG" 2>&1)"
case "$REFUSAL" in
  *"refuses rather than truncating"*) : ;;
  *) fail "daw-cli accepted a 40-byte name without explaining the limit — a caller would see
           'sent' and no change, which is the silent no-op this codebase keeps paying for.
           It said: $REFUSAL" ;;
esac

# THE ENGINE MUST REFUSE IT ON ITS OWN. `--oversize-anyway` skips the CLI's guard so the payload
# actually reaches the ring. Without this the check would only be testing daw-cli, and every
# other client — the web UI included — writes to the ring directly with no such guard.
ncli do sampler-slot-name --track 0 --device 1 --slot 3 --name "$LONG" --oversize-anyway \
  >/dev/null 2>&1
sleep 0.8
grep -q '"reason":"name_not_representable"' "$TMP/eng.log" 2>/dev/null || \
  fail "the engine did not refuse a 40-byte name — no name_not_representable in $TMP/eng.log.
        The published field holds 39 plus a terminator, so anything longer either truncates (and
        the save then disagrees with the screen forever) or runs off the end of the entry"
[ "$(pubname 3)" = "$BEFORE3" ] || \
  fail "a 40-byte name reached the engine and slot 3's published name changed from '$BEFORE3' to
        '$(pubname 3)'. Refusing means LEAVING IT ALONE — a truncated name is worse than none
        because it looks like it worked"
echo "  a 40-byte name is refused by both layers; slot 3 still reads '$(pubname 3)'"

# ---- PERSISTS.
ncli do save out >/dev/null 2>&1
sleep 1.5
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -f "$TMP/out.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"
python3 - "$TMP/out.uniproj.json" <<'PYC' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
names = {}
for t in d["tracks"]:
    for dev in t["device_chain"]:
        for s in dev.get("sampler", {}).get("slots", []):
            names[s["id"]] = s.get("name")
print("  saved names: %r" % names)
if names.get(1) != "BD_808":
    print("  FAIL: slot 1 saved %r, expected 'BD_808' — the loader's seed did not persist"
          % names.get(1)); raise SystemExit(1)
if names.get(2) != "kick tail":
    print("  FAIL: slot 2 saved %r, expected 'kick tail'. The rename reached the published"
          " read-back and not the save, so it is a name you set again every session."
          % names.get(2)); raise SystemExit(1)
if not names.get(3):
    print("  FAIL: slot 3 saved %r — a sliced slot's seeded name did not persist"
          % names.get(3)); raise SystemExit(1)
PYC

echo "slot_rename_check: PASS — load seeds the stem, a chop names every slice distinctly, a pad"
echo "                   can be renamed, only the pad named moves, an overlong name is refused"
echo "                   without touching the slot, and all of it survives the save"
