#!/usr/bin/env bash
# EVERY PER-TRACK FIELD THE ENGINE PUBLISHES CAN BE READ BACK — and reads back in the units it
# was set in.
#
# This repo has two ratchets for the WRITE direction: op_registry says every opcode has a way to
# send it, persisted_field_reach says every persisted field has something that writes it. There
# was none for the READ direction, and that is where the last two days of defects actually lived:
# `lines_per_beat` published since v10 and shown by nothing; a slot's `name` persisted since the
# sampler shipped and published by nothing; `harmony_quantize` recorded as unreadable in another
# agent's list for months while it sat in a byte they already read.
#
# When this was written the mixer (gain, pan, mute, solo), the lane quantize (grid, strength,
# swing) and the parentage (parent_id, has_parent) were all published every frame, all settable by
# command, and readable from NO surface at all. Nine controls whose own state the CLI could not
# show — which for an agent driving this CLI means it cannot verify the edit it just made.
#
# TWO HALVES, and the second is why this is not just a list-comparing script:
#   RATCHET    every ui*[kUiMaxTracks] array in shared_memory.h is named in the table below,
#              against the `get tracks` key that exposes it or an EXEMPT reason. A new published
#              array with no reader fails HERE rather than being noticed months later.
#   ROUND TRIP the values come back as SENT — gain in dB, pan in -1..1, swing SIGNED. Comparing
#              key names would pass on a read-back that reports millibels where dB were asked
#              for, or that leaks the wire's +500 swing bias. A mirror that only compares names
#              is blind to units.
#
#   tools/track_readback_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

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

# ---- RATCHET. Driven from the HEADER, so adding a published array without a reader fails here.
python3 - "$ROOT/apps/shared_memory.h" <<'PY' || exit 1
import re, sys
# Which `get tracks` key exposes each published per-track array. EXEMPT:<why> for the ones that
# genuinely should not surface — there are none today, and that is worth keeping true.
READER = {
    "uiTrackId":              "track_id",
    "uiTrackName":            "name",
    "uiTrackDeviceName":      "device",
    "uiTrackFlags":           "collapsed / master / absent / has_parent",
    "uiTrackMixFlags":        "harmony_quantize / sound_addressed / allow_note_overlap / mute / solo",
    "uiTrackGainMillibels":   "gain_db",
    "uiTrackPanThousandths":  "pan",
    "uiTrackPeakRms":         "peak_rms",
    "uiTrackOpsWidth":        "ops_width",
    "uiLinesPerBeat":         "lines_per_beat",
    "uiTrackParentId":        "parent_id",
    "uiTrackQuantizeGrid":    "quantize_grid",
    "uiTrackQuantizeStrength": "quantize_strength",
    "uiTrackQuantizeSwing":   "quantize_swing",
}
src = open(sys.argv[1]).read()
found = set(re.findall(r"\b(ui[A-Za-z]+)\[kUiMaxTracks\]", src))
missing = sorted(f for f in found if f not in READER)
stale = sorted(k for k in READER if k not in found)
if missing:
    print("  FAIL: %d per-track array(s) are published and named by no reader:" % len(missing))
    for m in missing:
        print("         %s" % m)
    print("        Give it a `get tracks` key, or add it to READER as EXEMPT:<why>. A field the")
    print("        engine publishes every frame and no surface can show is a control whose own")
    print("        state a UI has to invent — the defect this whole file exists for.")
    raise SystemExit(1)
if stale:
    print("  FAIL: the table names %d array(s) that no longer exist: %s" % (len(stale), ", ".join(stale)))
    print("        A ratchet held to a field that was deleted is not holding anything.")
    raise SystemExit(1)
print("  ratchet: all %d published per-track arrays have a named reader" % len(found))
PY

# ---- ROUND TRIP. The half a name comparison cannot make.
python3 - "$TMP/rt.uniproj.json" <<'PY'
import json, sys
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
tr = {"track_id":0,"name":"T","harmony_quantize":False,"lines_per_beat":4,
      "mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},
      "routing":{"midi_in":r(),"midi_out":r(),"audio_in":r(),
                 "audio_out":r("master"),"pre_fader_send":True},
      "device_chain":[],"mod_links":[],"placements":[]}
json.dump({"schema_version":4,"meta":{"name":"rt"},"nanoticks_per_quarter":960000,
           "tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],
           "clips":[],"tracks":[tr]}, open(sys.argv[1],"w"))
PY

SHM="/rtchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project rt --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

field() {  # field <key>
  cli get tracks 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print('unreadable'); raise SystemExit
t = d.get('tracks', [])
print(t[0].get('$1') if t else 'notracks')
" 2>/dev/null
}

# EVERY VALUE IS DELIBERATELY NOT A DEFAULT, and not symmetrical either: -6.5 dB rather than -6,
# pan 0.4 rather than 0.5, swing NEGATIVE. A read-back that returned zeroes, or that dropped a
# sign, or that reported the wire's biased swing (which would be 350 here) is caught by the value
# and not by its presence.
wait_for_published 30 "0.0" field gain_db || true
[ "$(field gain_db)" = "0.0" ] || fail "the fixture did not start at gain 0, so the change below
        proves nothing: it reads $(field gain_db)"
cli do mixer --track 0 --gain-db -6.5 --pan 0.4 --mute 1 >/dev/null 2>&1
cli do quantize --track 0 --grid 240000 --strength 750 --swing -150 >/dev/null 2>&1
for _ in $(seq 1 60); do
  [ "$(field gain_db)" = "-6.5" ] && [ "$(field quantize_swing)" = "-150" ] && break
  sleep 0.25
done

check() {  # check <key> <want>
  local got; got="$(field "$1")"
  [ "$got" = "$2" ] || fail "$1 reads '$got', expected '$2'. It is published every frame, so this
        is the READ-BACK disagreeing with what was set — either the units differ from the ones the
        command takes, or the value is not being surfaced at all"
}
check gain_db -6.5
check pan 0.4
check mute True
check solo False
check quantize_grid 240000
check quantize_strength 750
# THE SIGNED ONE. The wire carries swing +500-biased because the payload field is unsigned; the
# bias is the payload's business and must stop at the engine. A read-back of 350 here would mean
# it leaked all the way out to a caller who never sent it.
check quantize_swing -150
echo "  round trip: gain -6.5 dB, pan 0.4, mute, grid 240000, strength 750, swing -150 — all as sent"

# Parentage is published for every track and is FALSE/0 on a top-level one. Asserted rather than
# skipped: "the field is absent" and "the field says top-level" are different answers, and only
# one of them means the reader works.
check has_parent False
check parent_id 0
echo "  parentage: a top-level track says so, rather than the key being missing"

echo "track_readback_check: PASS — every published per-track field has a reader, and the values
  come back in the units they were sent in"
