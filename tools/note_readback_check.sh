#!/usr/bin/env bash
# EVERY ROW OP CAN BE READ BACK, AND THE ID THAT ADDRESSES ONE IS VISIBLE.
#
# `UiClipNote` publishes all six row ops — retrigger, probability, sound, sound offset, retrigger
# ramp, trig condition — and `get notes` printed NONE of them. `do set-row-ops` could write every
# one, so the only way to see what you had just done was to save the project and read the file.
# That is the same complaint sampler_kit_check's header makes about the kit, one feature along,
# and row ops are the tracker's core notation rather than a corner of it.
#
# WORSE THAN AN INCONVENIENCE: set-row-ops is ADDRESSED BY NOTE ID (`--note ID`) and `get notes`
# did not print note_id either. The write path needed an identifier the read path would not give
# you, so a caller had to already know it — which for an agent driving this CLI means it could not
# address a note it had just read.
#
# THREE PROPERTIES:
#   RATCHET     every field of the published UiClipNote is named against the `get notes` key that
#               shows it, or declared EXEMPT. Driven from the Rust mirror, so a field added to the
#               note fails HERE rather than being noticed when someone wants to read it
#   ADDRESSABLE note_id is printed, and the id it prints is the one set-row-ops accepts — asserted
#               by USING it, not by its presence
#   ROUND TRIP  all six ops come back as sent, including the SIGNED ramp. A name comparison would
#               pass on a read-back that dropped a sign or reported the wrong op entirely
#
# No audio device needed: the published note is SHM, not sound.
#   tools/note_readback_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

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
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# ---- RATCHET, driven from the Rust mirror of the published note.
python3 - "$ROOT/ui/daw-bridge/src/layout.rs" <<'PYR' || exit 1
import re, sys
READER = {
    "t_on": "nanotick",
    "t_off": "duration (derived here as t_off - t_on)",
    "note_id": "note_id",
    "pitch": "pitch",
    "velocity": "velocity",
    "column": "column",
    "retrigger": "retrigger",
    "probability": "probability",
    "placement_flags": "muted / is_add",
    "placement_id": "placement_id",
    "delay_nanoticks": "delay",
    "dev_nanoticks": "dev (and folded into sounds_at)",
    "sound": "sound",
    "sound_offset": "sound_offset",
    "retrig_ramp": "retrig_ramp",
    "trig_condition": "trig_condition",
    "reserved32": "EXEMPT:spare bytes, not a field — listed so their disappearance is noticed",
}
src = open(sys.argv[1]).read()
m = re.search(r"pub struct UiClipNote \{(.*?)\n\}", src, re.S)
if not m:
    print("  FAIL: could not find UiClipNote in the Rust mirror, so this ratchet is holding")
    print("        nothing. If the mirror moved, point this at its new home rather than deleting")
    print("        the check.")
    raise SystemExit(1)
fields = re.findall(r"^\s*pub ([a-z_0-9]+):", m.group(1), re.M)
missing = sorted(f for f in fields if f not in READER)
stale = sorted(k for k in READER if k not in fields)
if missing:
    print("  FAIL: %d published note field(s) are shown by no reader:" % len(missing))
    for x in missing:
        print("         %s" % x)
    print("        Give it a `get notes` key, or add it to READER as EXEMPT:<why>. A row op that")
    print("        can be WRITTEN and not READ sends a caller to the saved project file to find")
    print("        out what it just did.")
    raise SystemExit(1)
if stale:
    print("  FAIL: this table names %d field(s) the note no longer has: %s"
          % (len(stale), ", ".join(stale)))
    print("        A ratchet held to a field that was deleted is not holding anything.")
    raise SystemExit(1)
print("  ratchet: all %d published note fields have a named reader" % len(fields))
PYR

python3 - "$TMP/n.uniproj.json" "$Q" <<'PYF'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
# ONE note, and its note_id is deliberately NOT 1 — an id that happens to equal the index, or the
# count, or the first thing a loop would produce, cannot show that the printed id is the note's
# own. 42 is none of those.
notes = [{"nanotick": 0, "duration": Q, "pitch": 60, "velocity": 100, "column": 0, "note_id": 42}]
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "n"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic", "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PYF

SHM="/nrchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project n --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

note_field() {  # note_field <key>
  cli get notes --track 0 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print('unreadable'); raise SystemExit
n = d.get('notes', [])
print(n[0].get('$1') if n else 'nonotes')
" 2>/dev/null
}

# ---- ADDRESSABLE. The id is printed AND it is the one the write path takes.
wait_for_published 30 "42" note_field note_id || true
ID="$(note_field note_id)"
[ "$ID" = "42" ] || \
  fail "get notes reports note_id '$ID', and the fixture authored 42. Either the id is not
        published or it is not the note's own — and set-row-ops is addressed BY it, so a caller
        that cannot read it cannot name the note it just looked at"
echo "  addressable: the note's own id (42) is printed"

# ---- ROUND TRIP, using THAT id. If the printed id were wrong this is where it shows: the command
# would be refused and every op below would stay at zero.
cli do set-row-ops --track 0 --note "$ID" --ret 4 --prob 70 --sound 5 --offset 80 \
  --retrig-ramp -60 --condition 1:2 >/dev/null 2>&1
for _ in $(seq 1 60); do
  [ "$(note_field retrigger)" = "4" ] && break
  sleep 0.25
done

check() {  # check <key> <want>
  local got; got="$(note_field "$1")"
  [ "$got" = "$2" ] || fail "$1 reads '$got', expected '$2'. It is published in UiClipNote, so
        this is the read-back disagreeing with what set-row-ops wrote — or not surfacing it"
}
check retrigger 4
check probability 70
check sound 5
check sound_offset 80
# THE SIGNED ONE. rv-60 means the burst ENDS quieter; an unsigned read-back reports 196 and a
# caller would draw a crescendo where the note decays.
check retrig_ramp -60
# The PACKED code, not the "1:2" text — 1:2 encodes as 2. Asserted as the code because that is
# what is published; a UI decodes it, and pretending otherwise here would hide the encoding from
# the one place that documents it.
check trig_condition 2
echo "  round trip: all six ops come back as sent, ramp still signed, condition as its packed code"

# ---- AND THE PROVENANCE, which is not a row op but is the other thing a note carries that
# nothing showed. A plain clip note is neither muted nor an add; asserted so "the key is missing"
# and "the answer is false" stay distinguishable.
check muted False
check is_add False
check placement_id 1
echo "  provenance: the note reports its placement, and that it is neither muted nor an add"

echo "note_readback_check: PASS — every published row op reads back, and the id that addresses
  one is visible"
