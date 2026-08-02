#!/usr/bin/env bash
# A FOLDED TRACK STAYS FOLDED — the state can be SET, not only drawn.
#
# `collapsed` was persisted by the project format, published as kUiTrackFlagCollapsed, and
# restored into the runtime on load — so a hand-edited project round-tripped and a UI could DRAW
# the fold — and NO COMMAND COULD SET IT. A field the format claims to remember and nothing can
# write. It is the read-only-control complaint inverted: the state is visible and unreachable.
#
# Found by the same sweep that turned up the slot repoint one level down: every key project_file
# writes per TRACK, against every command. Most were covered — SetTrackMixer takes gain/pan/mute/
# solo, SetTrackRouting the routing, SetLaneQuantize the swing, SetTrackName the name — and the
# structural ones (parent_id, is_aux_child, aux_bus_index, is_master) are DERIVED by
# reconcileChildTracks and correctly have no command. This was the residue.
#
# THREE PROPERTIES:
#   STARTS OPEN   the published flag is clear before anything is sent. Without this, "it reads
#                 collapsed after the command" is equally consistent with a bit stuck on
#   SETS          the flag reads collapsed after opcode 89, so the state is reachable
#   PERSISTS      it survives a save, which is the half that makes the field worth having at all
#                 — a fold that is set and not saved is a fold you set again every session
#
#   tools/track_collapse_check.sh
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

# TWO TRACKS, and track 1 is never touched. A one-track fixture cannot tell "collapse track 0"
# from "collapse everything" — the same blind spot that let the kit read-back hand back another
# track's answer for months.
python3 - "$TMP/c.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def track(i, name):
    return {"track_id": i, "name": name, "harmony_quantize": False, "lines_per_beat": 4,
            "collapsed": False,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                        "audio_out": r("master"), "pre_fader_send": True},
            "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "c"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [track(0, "A"), track(1, "B")]}, open(out, "w"))
PY

SHM="/collapse_$$"
( cd "$BUILD" && exec env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" \
    ./daw_engine --project c --run-seconds 25 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 40
ccli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }

folded() {  # folded <trackIndex> — "true" / "false" from the PUBLISHED flag
  ccli get tracks 2>/dev/null | python3 -c "
import json, sys
t = json.load(sys.stdin).get('tracks', [])
print(str(t[int('$1')].get('collapsed')).lower() if len(t) > int('$1') else 'missing')"
}

# ---- STARTS OPEN.
wait_for_published 30 "false" folded 0 || true
wait_for_published 30 "false" folded 1 || true
[ "$(folded 0)" = "false" ] && [ "$(folded 1)" = "false" ] || \
  fail "a track reads collapsed before anything was sent (0=$(folded 0), 1=$(folded 1)). The
        published bit is stuck, so 'it reads collapsed after the command' below would prove
        nothing about the command"
echo "  before: track 0 $(folded 0), track 1 $(folded 1)"

# ---- SETS, and only the track named.
ccli do collapse --track 0 --on 1 >/dev/null 2>&1
sleep 0.6
grep -q '"event":"track.collapsed"' "$TMP/eng.log" 2>/dev/null || \
  fail "opcode 89 never reached the engine — no track.collapsed in $TMP/eng.log"
[ "$(folded 0)" = "true" ] || \
  fail "track 0 still reads collapsed=$(folded 0) after the command. The state is published and
        the command does not reach it — which is the same gap one step along from being unable
        to set it at all"
[ "$(folded 1)" = "false" ] || \
  fail "track 1 was never named and reads collapsed=$(folded 1). The command is collapsing more
        than the track it addresses, which a one-track fixture could never have shown"
echo "  after:  track 0 $(folded 0), track 1 $(folded 1)"

# ---- PERSISTS. A fold that is set and not saved is a fold you set again every session.
ccli do save out >/dev/null 2>&1
sleep 1.5
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -f "$TMP/out.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"
python3 - "$TMP/out.uniproj.json" <<'PYC' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
got = {t["track_id"]: t.get("collapsed") for t in d["tracks"]}
print("  saved: %r" % got)
if got.get(0) is not True:
    print("  FAIL: track 0 was collapsed and the save wrote %r. The save copies the runtime's"
          " atomic, so a command that set only the model — or set nothing — looks exactly like"
          " this." % got.get(0))
    raise SystemExit(1)
if got.get(1) is not False:
    print("  FAIL: track 1 was never collapsed and the save wrote %r" % got.get(1))
    raise SystemExit(1)
PYC

echo "track_collapse_check: PASS — a fold can be set, hits only the track it names, and survives"
echo "                      the save"
