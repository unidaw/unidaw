#!/usr/bin/env bash
# A KNOB YOU CANNOT DESCRIBE IS A KNOB YOU HAVE TO GUESS AT.
#
# The rack published a parameter's name and the TEXT of its current value, and nothing else. So a
# caller could read "Cutoff is 0.62, displays 440 Hz" and could not know:
#
#   * what 0.0 and 1.0 mean          -> setting 2 kHz is a binary search against the display text
#   * whether it is a SWITCH         -> writing 0.37 to a 5-position selector lands somewhere
#   * what the default is            -> "reset this" is not expressible
#   * whether it is automatable      -> an automation lane on a non-automatable param is a lie
#
# Every one of those fields was already collected by the JUCE wrapper (ParamInfo) on the first day
# and thrown away at the IPC boundary. M0.3 asked for it, calling it "nearly free and the
# difference between an assistant that can act on 'make the pad darker' and one that hallucinates".
#
# THE FIXTURE HAS TWO PARAMETERS THAT ARE UNLIKE EACH OTHER, and that is the point. Gain is
# continuous, automatable, in dB, ranging -60.0 to 0.0. Mode is a three-position switch with no
# unit, not automatable, whose value renders as a NAME. A fixture with one trivial parameter would
# pass an implementation that hardcoded empty strings and zero — the shape of test that proves
# nothing, which this repo has shipped before and now checks for on purpose.
#
# FOUR PROPERTIES:
#   DESCRIBES  unit, range endpoints, default, step count and flags are published per parameter
#   DISCRIMINATES  the two parameters differ in every one of those fields
#   PERSISTS   a manifest is written beside the opaque state blob, so a project read WITHOUT the
#              plugin installed and WITHOUT the engine running still says what the knobs were
#   ROUND TRIP the manifest survives a save and describes the same rack
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/param_metadata_check.sh
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
# THE ENGINE MUST DIE WHEN THIS CHECK DOES, including when ctest KILLS the check on a timeout.
# This trap used to remove $TMP and leave the engine running: it was only stopped on the normal
# path and inside fail(). A timed-out check therefore orphaned a possibly-hung engine, and ctest
# then blocked on it — measured at about 1000s per timeout across 18 runs, perfectly correlated
# with the timeout count. override showed it plainly: 909.87s against a TIMEOUT of 600, passing
# standalone in 23.2s.
#
# stop_engine escalates to SIGKILL after 10s and SAYS SO, so a hang stops being something to
# infer from a sample stack and becomes a line in the run.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
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
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

python3 - "$TMP/pm.uniproj.json" <<'PY'
import json, sys
DIRECT = 4294967294
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
dev = {"device_id": 0, "kind": "vst_instrument", "capability_mask": 5,
       "patcher_node_id": 0, "host_slot_index": DIRECT, "bypass": False,
       "vst_ref": {"vendor": "", "name": "identity", "path": "", "uid16": ""}}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [dev], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "pm"}, "nanoticks_per_quarter": 960000,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(sys.argv[1], "w"))
PY

SHM="/pmchk_$$"
( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 26 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load pm --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
# POLL FOR THE REGION THIS CHECK READS, not a fixed interval. wait_for_boot means the project
# LOADED; it does not mean the device-param region has been published — the gap that made
# lane_quantize flake one run in three.
#
# PARSES AND REQUIRES CONTENT rather than grepping a key. My first attempt grepped '"param"',
# which never matches (the array is "params"), so the poll timed out and the check failed —
# a predicate that can never be true is as bad as one that is always true.
params_ready() {
  cli get device-params 0 0 2>/dev/null | python3 -c 'import json,sys
try:
    d = json.load(sys.stdin)
except Exception:
    raise SystemExit(1)
raise SystemExit(0 if d.get("params") else 1)'
}
wait_until 20 params_ready || true

PARAMS="$(cli get device-params 0 0 2>/dev/null)"
echo "$PARAMS" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null || \
  fail "get device-params did not return JSON:
        $(echo "$PARAMS" | head -3)"

# field <name> <key> — read one parameter's field out of the published rack.
field() {
  echo "$PARAMS" | python3 -c "
import json, sys
d = json.load(sys.stdin)
for p in d['params']:
    if p['name'] == '$1':
        print(p['$2'])
        break
else:
    print('MISSING')
"
}

# ---- DESCRIBES. The continuous one, in the unit it declares, with a readable range.
[ "$(field Gain unit)" = "dB" ] || \
  fail "Gain's unit reads '$(field Gain unit)', expected dB. Without a unit, a caller setting a
        value in real terms has nothing to reason with"
GMIN="$(field Gain range | python3 -c "import sys,ast; print(ast.literal_eval(sys.stdin.read())[0])")"
GMAX="$(field Gain range | python3 -c "import sys,ast; print(ast.literal_eval(sys.stdin.read())[1])")"
[ "$GMIN" = "-60.0 dB" ] && [ "$GMAX" = "0.0 dB" ] || \
  fail "Gain's range endpoints read [$GMIN, $GMAX], expected [-60.0 dB, 0.0 dB]. These are the
        endpoints AS THE PLUGIN RENDERS THEM, which for a VST3 through JUCE is the only place its
        real range exists — the normalisable range is 0..1 and says nothing"
[ "$(field Gain default)" = "1.0" ] || \
  fail "Gain's default reads $(field Gain default), expected 1.0 — without it, 'reset this knob'
        cannot be expressed at all"
echo "  describes: Gain is dB, -60.0 to 0.0, default 1.0"

# ---- DISCRIMINATES. The switch differs from the knob in EVERY field, which is what makes this
# fixture able to fail. Its value renders as a NAME, not a number.
[ "$(field Mode discrete)" = "True" ] || \
  fail "Mode is a three-position switch and publishes discrete=$(field Mode discrete). Writing
        0.37 to a switch lands in whichever position that happens to be"
[ "$(field Mode steps)" = "3" ] || \
  fail "Mode publishes $(field Mode steps) steps, expected 3"
[ "$(field Mode automatable)" = "False" ] || \
  fail "Mode is not automatable and publishes automatable=$(field Mode automatable). An
        automation lane pointed at a parameter the plugin ignores is a lane that lies"
[ "$(field Mode display)" = "Off" ] || \
  fail "Mode's value renders as '$(field Mode display)', expected the position NAME 'Off'"
# ...and the knob is the opposite of the switch in all of them, so neither answer is hardcoded.
[ "$(field Gain discrete)" = "False" ] && [ "$(field Gain automatable)" = "True" ] && \
  [ "$(field Gain steps)" = "0" ] || \
  fail "Gain and Mode do not differ: an implementation returning one answer for every parameter
        would pass a fixture whose parameters are alike, which is why this one's are not"
echo "  discriminates: Mode is a 3-step switch, unautomatable, reading 'Off' — Gain is none of those"

# ---- PERSISTS. The manifest lands beside the opaque blob.
cli do save pmout --force >/dev/null 2>&1 || true
sleep 1.8
MANIFEST="$TMP/pmout.uniproj.state/t0_d0.params.json"
[ -f "$MANIFEST" ] || \
  fail "no parameter manifest at $MANIFEST. The state blob beside it is the plugin's private
        data and says nothing to anyone but the plugin — a project opened without this plugin
        installed would have no way to say what the knobs even were:
        $(ls "$TMP/pmout.uniproj.state" 2>/dev/null | tr '\n' ' ')"
grep -q '"event":"project.state_captured"' "$TMP/eng.log" || fail "no state_captured event"
grep '"event":"project.state_captured"' "$TMP/eng.log" | tail -1 | grep -q '"params_manifested":2' || \
  fail "the save did not report manifesting 2 parameters:
        $(grep -o '"event":"project.state_captured"[^}]*' "$TMP/eng.log" | tail -1)"
echo "  persists: a manifest is written beside the blob and the save reports it"

# ---- AND IT IS READABLE WITHOUT THE ENGINE. That is the whole point: this file is what an
# agent, a linter or a future you reads when the plugin is not installed and nothing is running.
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
OFFLINE="$(python3 - "$MANIFEST" <<'PYM'
import json, sys
d = json.load(open(sys.argv[1]))
by = {p["name"]: p for p in d["params"]}
g, m = by.get("Gain", {}), by.get("Mode", {})
print("%s|%s..%s|%s|%s|%s" % (g.get("unit"), g.get("min"), g.get("max"),
                              m.get("discrete"), m.get("steps"), m.get("automatable")))
PYM
)"
[ "$OFFLINE" = "dB|-60.0 dB..0.0 dB|True|3|False" ] || \
  fail "the manifest read offline says [$OFFLINE], expected [dB|-60.0 dB..0.0 dB|True|3|False].
        A manifest that needs the engine to interpret it is not a manifest"
echo "  offline: the manifest describes the rack with nothing running ($OFFLINE)"

echo "param_metadata_check: PASS — a parameter says what it IS, published and persisted"
