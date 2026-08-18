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
# The engine starts with NO --project, so it never emits a project.load at boot and the
# default wait_for_boot pattern would sit here until it timed out. "UI: command thread
# started" is the marker that means "ready to be told something", which is what the next
# line does. (Getting this pattern wrong is a mistake this repo has made three times.)
wait_for_boot "$TMP/engine.log" "$ENG" 80 "UI: command thread started" >/dev/null 2>&1 || true
DAW_UI_SHM_NAME="$SHM" "$CLI" do load src --force >/dev/null 2>&1 || true; sleep 0.8
DAW_UI_SHM_NAME="$SHM" "$CLI" do save v4a --force >/dev/null 2>&1 || true; sleep 0.8
DAW_UI_SHM_NAME="$SHM" "$CLI" do load v4a --force >/dev/null 2>&1 || true; sleep 0.8
DAW_UI_SHM_NAME="$SHM" "$CLI" do save v4b --force >/dev/null 2>&1 || true; sleep 0.8

# ---- THE SONG-LEVEL SCALARS, which nothing checked until two sabotages walked past the whole
# suite: the saved time signature forced to 7/4 whatever the song, and the saved generation seed
# forced to a constant. Both reach the file through this path and neither was observed by anything.
#
# THE SEED IS THE ONE THAT MATTERS. Every patcher generator folds it into its hash, so a project
# whose seed does not survive a save regenerates DIFFERENT MATERIAL next session — silently, and
# only for generated parts, which is the hardest kind of difference to notice.
#
# It needs its own source because the preset above is schema_version 1 and carries neither field,
# so a source-vs-saved comparison could not reach them however carefully it was written. This one
# is generated rather than added to presets/: a fixture that exists to be round-tripped does not
# need to be a project anyone opens, and the values are deliberately unusual (a seed nobody would
# arrive at by accident, a 7/8 that is not the 4/4 default) so a field that is being defaulted
# rather than carried is visible as itself.
python3 - "$TMP/seeded.uniproj.json" <<'PYS'
import json, sys
Q = 960000
json.dump({"schema_version": 4, "meta": {"name": "seeded"},
           "seed": 424242,
           "timebase": {"nanoticks_per_quarter": Q,
                        "time_sig_numerator": 7, "time_sig_denominator": 8},
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}],
           "harmony_timeline": [], "clips": [], "tracks": []}, open(sys.argv[1], "w"))
PYS
DAW_UI_SHM_NAME="$SHM" "$CLI" do load seeded --force >/dev/null 2>&1 || true; sleep 0.8
DAW_UI_SHM_NAME="$SHM" "$CLI" do save v4c --force >/dev/null 2>&1 || true; sleep 0.8
wait "$ENG" 2>/dev/null || true

[ -f "$TMP/v4a.uniproj.json" ] && [ -f "$TMP/v4b.uniproj.json" ] || {
  echo "save_roundtrip_check: FAIL — engine did not write both saves"; rm -rf "$TMP"; exit 1; }
[ -f "$TMP/v4c.uniproj.json" ] || {
  echo "save_roundtrip_check: FAIL — engine did not write the seeded save"; rm -rf "$TMP"; exit 1; }

# Normalize the project name (it legitimately follows the save filename) before diffing.
#
# AND THE ARTIFACT DIGESTS, which are volatile for a reason worth writing down. A schema-6 document
# commits each plugin artifact by its SHA-256 (AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME), and the
# sequence here is load -> save -> LOAD -> save: the second capture comes from plugin instances
# that were torn down and recreated with the first save's blob pushed back into them. Measured on
# maximal: the manifests are byte-identical across both saves, and the blobs are not — the same
# size, ~200 bytes different, in the parameter values. Pushing a VST3 state chunk and re-capturing
# it does not round-trip byte-exactly, which is a property of the plugin and its wrapper rather
# than of the save.
#
# That was invisible until the document started carrying digests, so this is a fact the change
# EXPOSED rather than caused. Nothing is lost: the blob that travels is exactly the one the
# document names, and the module check proves it unpacks byte-identical.
#
# NORMALIZED PRECISELY, NOT WHOLESALE. `sha256`, `size` and `artifact_generation` are blanked;
# `track_id`, `device_id`, `kind` and `leaf` are NOT — so this check still fails if an artifact
# appears, vanishes, or changes which device it belongs to, which is what it is for. Blanking the
# whole inventory would have made the shape of the inventory unwatched.
norm() {
  sed -e 's/"name": "v4[ab]"/"name": "v4"/' \
      -e 's/"artifact_generation": "[0-9a-f]*"/"artifact_generation": "<volatile>"/' \
      -e 's/"sha256": "[0-9a-f]*"/"sha256": "<volatile>"/' \
      -e 's/"size": [0-9]*/"size": <volatile>/'
}
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
# THE SONG-LEVEL LISTS, which nothing checked until a sabotage walked straight through.
#
# Making the engine write a constant seed, or the wrong time signature, was invisible here: this
# comparison only ever looked at mod_links and devices. Anything else the load path drops is
# emitted as an empty array by the first save and deleted from disk — which is the exact failure
# the mod_links comparison above was written for, left unguarded for its neighbours.
#
# tempo_map and harmony_timeline are in the fixture (2 and 4 entries), so a drop is measurable
# rather than hypothetical.
def tempo(d):
    return len(d.get('tempo_map', []) or [])
def harmony(d):
    return len(d.get('harmony_timeline', []) or [])
ok = True
for name, fn in (("mod_links", modlinks), ("devices", devices),
                 ("tempo_map", tempo), ("harmony_timeline", harmony)):
    before, after = fn(a), fn(b)
    # The master track is appended on save, so devices may legitimately GROW; they must
    # never shrink. Mod links must be preserved exactly.
    if name in ("mod_links", "tempo_map", "harmony_timeline") and after != before:
        print(f"  FAIL: {name} {before} -> {after} (the load path is dropping them)")
        ok = False
    elif name == "devices" and after < before:
        print(f"  FAIL: {name} {before} -> {after} (devices lost in the round trip)")
        ok = False
    else:
        print(f"  {name} preserved: {before} -> {after}")
raise SystemExit(0 if ok else 1)
PY

# ---- NO LIST MAY SHRINK, and this is the rule that stops the previous three from being a list
# somebody extends whenever a drop is noticed.
#
# mod_links, devices, tempo_map and harmony_timeline were each added after a gap was found the
# hard way. Meanwhile the fixture's 158 NOTES, its chords, and its patcher nodes and edges were
# never compared at all — and a load path that dropped them would sail through, because both saves
# lose them identically and the save-vs-save diff stays clean. That is the exact blindness the
# mod_links comparison was written for, left open for everything nobody had happened to lose yet.
#
# So: count every array in the document by its JSON path and require that none of them shrinks.
# A field nobody has thought about is covered by the same rule as the ones that were.
#
# COUNTED BY LEAF NAME, NOT BY PATH, and that distinction is the whole correctness of it. The
# source is schema_version 1, which stores notes and chords ON THE TRACK; a schema-4 save stores
# them in CLIPS, because clips-plus-placements is the note store. Keyed by full path this rule
# reported "/tracks[]/notes 158 -> 0" and called it a loss of 158 notes — they had moved to
# /clips[]/notes, all 158 of them. A rule that cannot tell relocation from deletion is worse than
# no rule: it manufactures an alarming and false finding, which is exactly the kind that gets a
# check disabled.
#
# GROWTH IS ALLOWED, because the master track is appended on save and legitimately adds a track
# and its devices. Shrinkage never is: nothing in a faithful round trip should come back with
# fewer of anything, wherever the schema chooses to keep it.
python3 - "$TMP/src.uniproj.json" "$TMP/v4a.uniproj.json" <<'PYS' || ok=0
import json, sys, collections
def load(p):
    d = json.load(open(p))
    return d.get('document', d)
def counts(node, name='', acc=None):
    if acc is None: acc = collections.Counter()
    if isinstance(node, dict):
        for k, v in node.items():
            counts(v, k, acc)
    elif isinstance(node, list):
        acc[name] += len(node)
        for v in node:
            counts(v, name, acc)
    return acc
a, b = counts(load(sys.argv[1])), counts(load(sys.argv[2]))
ok = True
shrunk = [(k, a[k], b.get(k, 0)) for k in sorted(a) if b.get(k, 0) < a[k]]
for k, before, after in shrunk:
    print(f"  FAIL: {k} {before} -> {after} — the round trip lost {before - after}")
    ok = False
if ok:
    print("  no list shrank across the round trip (%d kinds, %d entries)"
          % (len(a), sum(a.values())))
raise SystemExit(0 if ok else 1)
PYS

# The scalars, compared against the values the source stated rather than against the other save.
python3 - "$TMP/seeded.uniproj.json" "$TMP/v4c.uniproj.json" <<'PYS' || ok=0
import json, sys
def load(p):
    d = json.load(open(p))
    return d.get('document', d)
a, b = load(sys.argv[1]), load(sys.argv[2])
ok = True
checks = [
    ("seed", a.get("seed"), b.get("seed"),
     "every patcher generator folds the seed into its hash, so a seed that does not survive a "
     "save regenerates different material next session"),
    ("time_sig_numerator", a["timebase"]["time_sig_numerator"],
     b.get("timebase", {}).get("time_sig_numerator"),
     "the ruler, note entry and every bar boundary read this"),
    ("time_sig_denominator", a["timebase"]["time_sig_denominator"],
     b.get("timebase", {}).get("time_sig_denominator"), "same"),
]
for name, before, after, why in checks:
    if before != after:
        print(f"  FAIL: {name} {before} -> {after} on save — {why}")
        ok = False
    else:
        print(f"  {name} survives the engine: {before}")
raise SystemExit(0 if ok else 1)
PYS

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
