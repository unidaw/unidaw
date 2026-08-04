#!/usr/bin/env bash
# THE PUBLISHED METER POINTS ARE INTERNALLY INCONSISTENT, AND THIS PINS IT.
#
# `UiArrangeSummaryRegion.timeSigPoints` publishes `points()[i].nanotick` — the tick a signature
# change was ASKED FOR. TimeSignatureMap::setMap then snaps every change FORWARD to the next bar
# line of the preceding signature, because a change landing mid-bar would leave a partial bar and
# "bar 9" would mean two different things depending which side you counted from. That snapped
# position lives in `segments_`, which is never published.
#
# So a client drawing the ruler from this region places the change where it was requested rather
# than where it happens. Measured: 3/4 requested at 5 quarters comes back as 5 quarters, while the
# change actually begins at 8 — three quarters early, and every bar line derived after it inherits
# the error. It looks like the CLIENT's arithmetic, because the client's arithmetic is fine.
#
# THE ASSERTION NEEDS NO KNOWLEDGE OF THE SNAPPING RULE, which is what makes it worth having. A
# meter change must sit a WHOLE NUMBER OF PRECEDING BARS after the point before it — that is what
# "no partial bars" means, and it is checkable from the published points alone. Recomputing the
# snap here would be a second copy of the rule under test, and this repo has spent a night finding
# out what those cost.
#
# THIS CHECK IS INVERTED. It asserts the violation is PRESENT, because the fix is a contract change
# (publishing the effective tick, plus a resolved bar number so clients need not re-derive the
# prefix sum) that bumps kShmVersion and forces a rebuild on both sides. That is an owner's call
# and a coordination question, not something to land quietly at night. Until then the defect is at
# least measured, and cannot drift.
#
# WHEN THIS GOES RED, READ THE MESSAGE. If the published point has become a whole number of bars,
# the wire now carries effective ticks and this check has done its job: invert it to assert the
# invariant holds, and delete this paragraph.
#
#   tools/meter_publish_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
. "$ROOT/tools/lib/engine_wait.sh"
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

# 4/4 from the start, then 3/4 REQUESTED at five quarters — deliberately mid-bar, since 4/4 bars
# fall at 0, 4 and 8 quarters. A request already on a bar line would be snapped to itself and the
# defect would be invisible.
python3 - "$TMP/meterpub.uniproj.json" <<'PY'
import json, sys
Q = 960000
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": 8 * Q,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "meterpub"}, "nanoticks_per_quarter": Q,
           "seed": 1,
           "timebase": {"nanoticks_per_quarter": Q, "time_sig_numerator": 4,
                        "time_sig_denominator": 4},
           "time_sig_map": [{"nanotick": 0, "numerator": 4, "denominator": 4},
                            {"nanotick": 5 * Q, "numerator": 3, "denominator": 4}],
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "c", "length": 8 * Q, "kind": "symbolic", "notes": []}],
           "tracks": [tr]}, open(sys.argv[1], "w"))
PY

SHM="/meterpub_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project meterpub --run-seconds 25 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
sleep 0.8
DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" get arrangement >"$TMP/arr.json" 2>&1 \
  || fail "get arrangement failed — see $TMP/arr.json"

python3 - "$TMP/arr.json" "$Q" <<'PY' || exit 1
import json, re, sys
Q = int(sys.argv[2])
text = open(sys.argv[1]).read()
try:
    doc = json.loads(text)
except Exception:
    print("  FAIL: get arrangement did not return JSON:"); print(text[:300]); raise SystemExit(1)
pts = doc.get("time_sig", [])

# VACUITY GUARD FIRST. Every assertion below is about the SECOND point; if the fixture's meter
# never reached the wire there is nothing to be wrong, and a check that passes on an empty list
# would be measuring the absence of its own input.
if len(pts) != 2:
    print(f"  FAIL: the wire published {len(pts)} meter point(s), expected 2 ({pts}).")
    print( "        The fixture states two, so this is the publish path dropping one rather than")
    print( "        anything about where they sit")
    raise SystemExit(1)
def sig(p):
    m = re.match(r'(\d+)/(\d+)$', p["sig"])
    return int(m.group(1)), int(m.group(2))
n0, d0 = sig(pts[0])
n1, d1 = sig(pts[1])
if (n0, d0) != (4, 4) or (n1, d1) != (3, 4):
    print(f"  FAIL: published signatures are {pts[0]['sig']} then {pts[1]['sig']}, expected 4/4 then 3/4")
    raise SystemExit(1)
print(f"  published: {pts[0]['sig']} at {pts[0]['nanotick']}, {pts[1]['sig']} at {pts[1]['nanotick']}")

# THE INVARIANT: a change sits a whole number of PRECEDING bars after the point before it.
bar0 = n0 * (4 * Q // d0)             # a 4/4 bar, in nanoticks
delta = pts[1]["nanotick"] - pts[0]["nanotick"]
bars = delta / bar0
if delta % bar0 == 0:
    print(f"  FAIL: the second point is {bars:.0f} whole bars in — the invariant now HOLDS.")
    print( "        READ THIS BEFORE ASSUMING A REGRESSION: this check is INVERTED. If the wire")
    print( "        now publishes the tick where the change TAKES EFFECT rather than the one it")
    print( "        was requested at, the contract fix has landed and this check has done its")
    print( "        job — invert it to assert the invariant and delete the inverted paragraph in")
    print( "        its header.")
    raise SystemExit(1)
print(f"  KNOWN DEFECT pinned: the change is published {bars:.2f} bars after the previous point,")
print( "    which is not a whole number of bars — so the published tick is the one the change was")
print( "    REQUESTED at, not the one it takes effect at. setMap snaps forward to the next bar")
print(f"    line ({bars:.2f} -> {-(-bars // 1):.0f} bars) and that snapped position is never published.")
print( "    Task #2; the fix bumps kShmVersion and needs coordination with the UI.")
PY

echo "meter_publish_check: PASS — the published meter is measured, and its inconsistency is pinned"
