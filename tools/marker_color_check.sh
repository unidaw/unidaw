#!/usr/bin/env bash
# A MARKER'S COLOUR CAN BE CHANGED — AND THE CHANGE IS SEEN AND SAVED.
#
# It was write-once. `UiMarkerCommandPayload` has carried `colorRgb` since v29 and AddMarker
# assigned it; RenameMarker set only the name and MoveMarker only the tick, so a marker created
# with the wrong colour kept it forever and no surface could recolour it. Persisted AND published,
# so something depended on a field nothing could write — found by giving persisted_field_reach a
# MARKER scope, which is also what made it the last GAP in that table.
#
# Opcode 99 reuses the existing payload, so nothing about the wire changed.
#
# THE PROPERTY THAT DECIDED THE DESIGN is BLACK. Every 24-bit value is a legal colour, including
# 0, so "no colour supplied" cannot be distinguished from "make it black" once the payload is on
# the ring. That is why this is its own opcode instead of a flag on RenameMarker: a rename
# carrying colour would paint every renamed marker black whenever a caller left the field alone,
# silently, and it would look like a UI bug. The BLACK property below is the assertion that the
# distinction is real — it recolours a non-black marker TO black and requires that it took.
#
# SIX PROPERTIES:
#   SEEN         the published arrangement read-back reports the new colour
#   SAVED        and the project file does too — the format is where the GAP was declared
#   ADDRESSED    two markers, recolour one, the other keeps its colour. A fixture with one marker
#                cannot tell a command that addresses correctly from one that edits whatever it
#                finds first
#   INDEPENDENT  a recolour changes ONLY the colour: name and tick are asserted unchanged, because
#                this rides a payload that also carries both and a handler reading the wrong field
#                would move the marker to tick 0
#   BLACK        recolouring to 0 WORKS and is not read as "unchanged" (see above)
#   REFUSED      an id that does not exist is refused by the ENGINE with no_such_marker, read from
#                its log rather than from daw-cli's exit code — the CLI validates SHAPE and the
#                engine validates DOMAIN, and the web UI's sidecar writes the ring directly
#
# Model and shared memory only: no audio device, no render.
#   tools/marker_color_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }
say() { [ "$(eval "echo \$$1")" = "1" ] && echo "  $2"; return 0; }

# TWO MARKERS, ids 3 and 8 — neither is 1, so an id equal to an index or a count cannot pass by
# coincidence, and they start at DIFFERENT colours so "the other one is unchanged" is a real
# observation rather than two zeros agreeing.
python3 - "$TMP/mk.uniproj.json" <<'PY'
import json, sys
Q = 960000
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "mk"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr],
           "markers": [{"id": 3, "nanotick": 0, "name": "Intro", "color_rgb": 111111},
                       {"id": 8, "nanotick": 3840000, "name": "Verse", "color_rgb": 222222}]},
          open(sys.argv[1], "w"))
PY

SHM="/mkcol_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project mk --run-seconds 120 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

# One marker's three mutable fields out of the PUBLISHED read-back, together — so a command that
# changed the colour by moving the marker cannot read as a success.
mk() {  # mk <id> -> "<color> <nanotick> <name>"
  cli get arrangement 2>/dev/null | python3 -c "
import re, sys
want = $1
for line in sys.stdin:
    if '\"id\": %d,' % want not in line:
        continue
    c = re.search(r'\"color_rgb\": (\d+)', line)
    t = re.search(r'\"nanotick\": (\d+)', line)
    n = re.search(r'\"name\": \"([^\"]*)\"', line)
    if c and t and n:
        print('%s %s %s' % (c.group(1), t.group(1), n.group(1)))
        raise SystemExit
print('missing')
" 2>/dev/null
}
waitmk() {  # waitmk <id> <want>
  for _ in $(seq 1 60); do
    [ "$(mk "$1")" = "$2" ] && return 0
    sleep 0.25
  done
  return 1
}
reason() {
  grep '"event":"marker.rejected"' "$TMP/eng.log" 2>/dev/null | tail -1 |
    python3 -c "import re,sys; t=sys.stdin.read(); m=re.search(r'\"reason\":\"([^\"]*)\"', t); print(m.group(1) if m else 'none')"
}

ok=1
[ "$(mk 3)" = "111111 0 Intro" ] || fail "marker 3 reads '$(mk 3)', not the fixture's
        '111111 0 Intro' — so every assertion below would prove nothing"
[ "$(mk 8)" = "222222 3840000 Verse" ] || fail "marker 8 reads '$(mk 8)'"
echo "  both markers publish their fixture colours"

# ---- SEEN + INDEPENDENT. The tick and name are in the same assertion on purpose: this command
# rides a payload carrying all three, and a handler reading the wrong field would move the marker.
cli do marker color --id 3 --color 16711680 >/dev/null 2>&1
waitmk 3 "16711680 0 Intro" || \
  fail "marker 3 reads '$(mk 3)' after a recolour, wanted '16711680 0 Intro'. If only the colour
        is wrong the publish did not follow the edit; if the TICK or NAME moved, the handler read
        the wrong field out of a payload that carries all three"
echo "  seen: marker 3 is 16711680, and its tick and name did not move"

# ---- ADDRESSED.
[ "$(mk 8)" = "222222 3840000 Verse" ] || \
  { echo "  FAIL: recolouring marker 3 also changed marker 8 to '$(mk 8)' — the command is
        editing by position, or whatever it finds first, rather than by id"; ok=0; }
say ok "addressed: marker 8 was left alone"

# ---- BLACK. The property the whole design rests on: 0 is a COLOUR, not a sentinel for
# "unchanged". A handler treating a zero colour as "leave it alone" passes every other assertion
# on this page and fails this one.
blackok=1
cli do marker color --id 3 --color 0 >/dev/null 2>&1
waitmk 3 "0 0 Intro" || \
  { echo "  FAIL: recolouring marker 3 to BLACK left it at '$(mk 3)'. Zero is a legal colour and
        must not be read as 'no colour supplied' — if it is, then a rename could never safely
        carry a colour, which is exactly why this is its own opcode"; ok=0; blackok=0; }
say blackok "black: 0 is applied as a colour, not read as 'unchanged'"

# ---- REFUSED, through the ENGINE. `cli ... && fail` would assert daw-cli's copy of the rule and
# would still pass with the engine's guard deleted.
refok=1
cli do marker color --id 4242 --color 5 >/dev/null 2>&1
sleep 0.6
[ "$(reason)" = "no_such_marker" ] || \
  { echo "  FAIL: recolouring a marker that does not exist was not refused with no_such_marker
        (got '$(reason)')"; ok=0; refok=0; }
say refok "refused: no_such_marker"

# ---- SAVED. Where the GAP was declared — a command that does not reach the file has not closed
# it. Recoloured to something non-zero first, so a save writing a default zero cannot pass.
cli do marker color --id 3 --color 5592405 >/dev/null 2>&1
waitmk 3 "5592405 0 Intro" || { echo "  FAIL: the third recolour did not take"; ok=0; }
cli do save mk_out >/dev/null 2>&1
sleep 1.5
python3 - "$TMP/mk_out.uniproj.json" <<'PY' || ok=0
import json, sys
doc = json.load(open(sys.argv[1]))
m = {x["id"]: x for x in doc.get("markers", [])}
bad = 0
if not m:
    print("  FAIL: the saved project has no markers at all"); raise SystemExit(1)
if m[3].get("color_rgb") != 5592405:
    print("  FAIL: saved marker 3 has color_rgb %r, wanted 5592405" % m[3].get("color_rgb"))
    bad = 1
if m[3].get("name") != "Intro" or m[3].get("nanotick") != 0:
    print("  FAIL: saved marker 3 moved or was renamed: %r" % m[3]); bad = 1
if m[8].get("color_rgb") != 222222:
    print("  FAIL: saved marker 8 changed colour to %r" % m[8].get("color_rgb")); bad = 1
raise SystemExit(bad)
PY
say ok "saved: the recolour is in the project file, and marker 8 is untouched"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ "$ok" = "1" ] && echo "marker_color_check: PASS — a marker's colour is settable, seen and saved" \
                || { echo "marker_color_check: FAIL"; exit 1; }
