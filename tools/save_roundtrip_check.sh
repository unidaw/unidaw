#!/usr/bin/env bash
# ARCHITECTURE_REVIEW #7 / M0.7: a project round-trips THROUGH THE ENGINE. Load a
# project, save it (v4a), load THAT, save again (v4b); the two schema-4 saves must be
# byte-identical apart from the project name (which is derived from the save filename).
# This catches the engine's live document drifting from what it serializes — e.g. a
# runtime vst_ref stamped onto a patcher device, or a clip/placement injected on load.
# The one-time schema-1 -> schema-4 upgrade is expected and is NOT what this asserts;
# idempotency of the schema-4 form is.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/save_roundtrip_check.sh [preset-name]   (default: maximal)
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
NAME="${1:-maximal}"
SHM="/save_rt_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }
[ -f "$ROOT/presets/projects/$NAME.uniproj.json" ] || { echo "no such preset: $NAME"; exit 2; }

TMP="$(mktemp -d)"
cp "$ROOT/presets/projects/$NAME.uniproj.json" "$TMP/src.uniproj.json"

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 12 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
# THE ENGINE GOES WITH THE CHECK, including when ctest KILLS it on a timeout. Without this the
# engine was orphaned and ctest then blocked on it — ~1000s per timeout, measured across 18 runs.
# stop_engine escalates to SIGKILL after 10s and says so.
# There was NO EXIT TRAP here at all, so a timed-out check was guaranteed to orphan.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
sleep 2.5
DAW_UI_SHM_NAME="$SHM" "$CLI" do load src --force >/dev/null 2>&1 || true; sleep 0.8
DAW_UI_SHM_NAME="$SHM" "$CLI" do save v4a --force >/dev/null 2>&1 || true; sleep 0.8
DAW_UI_SHM_NAME="$SHM" "$CLI" do load v4a --force >/dev/null 2>&1 || true; sleep 0.8
DAW_UI_SHM_NAME="$SHM" "$CLI" do save v4b --force >/dev/null 2>&1 || true; sleep 0.8
wait "$ENG" 2>/dev/null || true

[ -f "$TMP/v4a.uniproj.json" ] && [ -f "$TMP/v4b.uniproj.json" ] || {
  echo "save_roundtrip_check: FAIL — engine did not write both saves"; rm -rf "$TMP"; exit 1; }

# Normalize the project name (it legitimately follows the save filename) before diffing.
norm() { sed 's/"name": "v4[ab]"/"name": "v4"/'; }
ok=1
if diff <(norm < "$TMP/v4a.uniproj.json") <(norm < "$TMP/v4b.uniproj.json") >/dev/null 2>&1; then
  echo "  schema-4 save->load->save is byte-identical"
else
  echo "  FAIL: schema-4 round-trip is NOT idempotent:"
  diff <(norm < "$TMP/v4a.uniproj.json") <(norm < "$TMP/v4b.uniproj.json") | head -20
  ok=0
fi

# Content that must SURVIVE the trip, compared against the ORIGINAL rather than against
# the other save. Save-vs-save idempotency alone is blind to anything the LOAD drops: both
# sides lose it identically and the diff stays clean. That is exactly how a load path that
# parsed mod_links and never installed them went unnoticed — the first save silently
# emitted an empty array and deleted them from disk.
python3 - "$TMP/src.uniproj.json" "$TMP/v4a.uniproj.json" <<'PY' || ok=0
import json, sys
def load(p):
    d = json.load(open(p))
    return d.get('document', d)
a, b = load(sys.argv[1]), load(sys.argv[2])
def modlinks(d):
    return sum(len(t.get('mod_links', []) or []) for t in d.get('tracks', []))
def devices(d):
    return sum(len(t.get('device_chain', []) or []) for t in d.get('tracks', []))
ok = True
for name, fn in (("mod_links", modlinks), ("devices", devices)):
    before, after = fn(a), fn(b)
    # The master track is appended on save, so devices may legitimately GROW; they must
    # never shrink. Mod links must be preserved exactly.
    if name == "mod_links" and after != before:
        print(f"  FAIL: {name} {before} -> {after} (the load path is dropping them)")
        ok = False
    elif name == "devices" and after < before:
        print(f"  FAIL: {name} {before} -> {after} (devices lost in the round trip)")
        ok = False
    else:
        print(f"  {name} preserved: {before} -> {after}")
raise SystemExit(0 if ok else 1)
PY

# A patcher/instrument/audio patcher device must never carry a plugin vst_ref.
bad="$(python3 -c "
import json
d=json.load(open('$TMP/v4a.uniproj.json')); doc=d.get('document',d)
print(len([1 for t in doc['tracks'] for dv in t.get('device_chain',[])
          if dv['kind'].startswith('patcher') and dv.get('vst_ref',{}).get('name')]))
")"
if [ "$bad" != "0" ]; then
  echo "  FAIL: $bad patcher device(s) carry a bogus vst_ref"; ok=0
else
  echo "  no patcher device carries a plugin vst_ref"
fi

rm -rf "$TMP"
[ "$ok" = "1" ] && echo "save_roundtrip_check: PASS — through-engine save is faithful" \
                || { echo "save_roundtrip_check: FAIL"; exit 1; }
