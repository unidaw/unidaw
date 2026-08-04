#!/usr/bin/env bash
# A PROJECT BUILT BY EDITING, round-tripped. The fixture is not authored — it is produced by a
# session's worth of commands, and then the file that session wrote has to survive a reload.
#
# WHY THIS EXISTS, and it is the most useful thing found in a night of bug hunting: every other
# fixture in this repo is AUTHORED. Someone wrote the JSON by hand, so ids are dense from zero,
# nothing is tombstoned, no placement carries an override, and the tracks number six because six
# was enough to write. Two data-loss bugs hid behind exactly that shape and neither was found by
# 44 unit tests or 35 end-to-end checks:
#
#   - a project saved after a track was REMOVED has sparse ids, and the load treated the track
#     count as the id extent — destroying the highest track and inventing a phantom one. No
#     fixture has a tombstone, because you do not author a hole.
#   - the "maximal" fixture that save_roundtrip_check calls faithful has zero markers, zero
#     automation and zero placement overrides. It predates three Movements. So "through-engine
#     save is faithful" had never round-tripped any of Movement 3.
#
# The condition for both was a REMOVAL followed by a SAVE, or a feature used and then saved —
# things only a session does. So this check does what a person does: it edits, saves, reloads in
# a FRESH engine, saves again, and asserts both that the specific state survived and that the
# second save is a fixed point.
#
# BOTH assertions are needed and neither is sufficient:
#   FIXED POINT   save -> reload -> save must be stable. Catches a field that mutates on every
#                 round trip (a normalisation that is not idempotent).
#   PRESENT       each thing the edits created must still be there. Catches data LOSS, which a
#                 fixed-point test cannot see — a field dropped at save is stably absent
#                 afterwards, so B == C passes while the data is gone.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/edited_roundtrip_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((4 * Q))

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

# A plain, small, entirely ordinary starting point. Everything interesting is done to it by the
# commands below, which is the whole point.
python3 - "$TMP/start.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = 4 * Q
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
def track(tid, name, clip_id):
    return {"track_id": tid, "name": name, "harmony_quantize": False, "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": routing(), "device_chain": [], "mod_links": [],
            "placements": [{"clip_id": clip_id, "id": 10 * tid + 1, "at": 0,
                            "length": 4 * BAR, "notes": [], "chords": [], "mutes": []}]}
clips = [{"id": c, "name": "c%d" % c, "length": 4 * BAR, "kind": "symbolic",
          "notes": [{"nanotick": 0, "duration": Q, "pitch": 60 + c, "velocity": 100,
                     "column": 0, "note_id": c}]} for c in (1, 2, 3)]
json.dump({"schema_version": 4, "meta": {"name": "start"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": clips,
           "tracks": [track(0, "Kick", 1), track(1, "Snare", 2), track(2, "Hat", 3)]},
          open(out, "w"))
PY

start_engine() {  # $1=shm  $2=logfile
  ( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$1" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --run-seconds 40 >"$2" 2>&1 ) &
  ENG=$!
  wait_for_boot "$2" "$ENG" 160 "starting threads"
}
# Counts loads because this check reloads the same engine twice. The callers below already say
# `|| fail`, so the outcome was asserted — what it could not do was notice the engine DYING while
# it waited, and then all three call sites reported "never loaded" for a process that had exited.
# wait_for_loads fails immediately with the log tail instead. Task #106.
wait_load() {  # $1=logfile  $2=count  $3=pid  $4=what we are waiting for
  wait_for_loads "$1" "${3:-}" "$2" 80 "${4:-a project load}"
}

# ---- THE SESSION. Each command is a thing a person does, and each one exercises a different
# persisted structure. Order matters in one place: the track is removed AFTER being edited, so
# the tombstone sits in the middle of material rather than at the end.
SHM="/edrt1_$$"
start_engine "$SHM" "$TMP/eng1.log"
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load start --force >/dev/null 2>&1 || true
wait_load "$TMP/eng1.log" 1 "$ENG" "the starting project to load"
sleep 1

cli do rename --track 0 --name "Kick Redone"      >/dev/null 2>&1 || true; sleep 0.5
cli do add-track --force                          >/dev/null 2>&1 || true; sleep 0.6
cli do note --track 0 --nanotick $((1 * BAR)) --pitch 40 --duration $Q \
                                                  >/dev/null 2>&1 || true; sleep 0.6
cli do note --track 2 --local --nanotick $((2 * BAR)) --pitch 55 --duration $Q \
                                                  >/dev/null 2>&1 || true; sleep 0.6
cli do delete-note --track 2 --local --nanotick 0 --pitch 63 \
                                                  >/dev/null 2>&1 || true; sleep 0.6
cli do marker add --nanotick 0 --name intro       >/dev/null 2>&1 || true; sleep 0.5
cli do marker add --nanotick $((4 * BAR)) --name verse >/dev/null 2>&1 || true; sleep 0.5
cli do time-sig --sig 7/8 --nanotick $((4 * BAR)) >/dev/null 2>&1 || true; sleep 0.5
# On track 0, which SURVIVES. The first version of this wrote automation to track 1 and then
# removed track 1 — so the point was correctly gone and the check accused the engine of losing
# it. Writing to the track you are about to delete tests deletion, not persistence.
cli do automation --track 0 --param index:0 --nanotick $((1 * BAR)) --value 0.7 \
                                                  >/dev/null 2>&1 || true; sleep 0.6
cli do remove-track --track 1 --force             >/dev/null 2>&1 || true; sleep 0.6
cli do time insert --nanotick $((4 * BAR)) --bars 2 >/dev/null 2>&1 || true; sleep 0.8

cli do save editedA --force >/dev/null 2>&1 || true
sleep 1.8
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -f "$TMP/editedA.uniproj.json" ] || fail "the session's save produced no file"

# What the session should have produced, read out of A so the assertions below describe the
# state under test rather than a guess about it.
summary() {
  python3 - "$1" <<'PYS'
import json, sys
d = json.load(open(sys.argv[1]))
tracks = [t for t in d.get("tracks", []) if not t.get("is_master")]
ids = sorted(t.get("track_id") for t in tracks)
names = {t.get("track_id"): t.get("name") for t in tracks}
adds = sum(len(p.get("notes", [])) for t in tracks for p in t.get("placements", []))
mutes = sum(len(p.get("mutes", [])) for t in tracks for p in t.get("placements", []))
autos = sum(len(t.get("automation", [])) for t in tracks)
apts = sum(len(c.get("points", [])) for t in tracks for c in t.get("automation", []))
marks = [(m.get("name"), m.get("nanotick")) for m in d.get("markers", [])]
meter = [(p.get("nanotick"), "%d/%d" % (p.get("numerator"), p.get("denominator")))
         for p in d.get("time_sig_map", [])]
notes = sum(len(c.get("notes", [])) for c in d.get("clips", []))
print("ids=%s name0=%r adds=%d mutes=%d autoclips=%d autopoints=%d markers=%s meter=%s "
      "clipnotes=%d masters=%d"
      % (ids, names.get(0), adds, mutes, autos, apts, marks, meter, notes,
         sum(1 for t in d.get("tracks", []) if t.get("is_master"))))
PYS
}
A="$(summary "$TMP/editedA.uniproj.json")"
echo "  session wrote: $A"

# PRESENT, checked on A first: if the session's own save is already missing something, the
# round-trip assertions below would be comparing two copies of the same loss.
case "$A" in
  *"name0='Kick Redone'"*) ;;
  *) fail "the rename is not in the session's own save: $A" ;;
esac
case "$A" in
  *"adds=1"*)  ;;
  *) fail "the local ADD is not in the session's own save: $A" ;;
esac
case "$A" in
  *"mutes=1"*) ;;
  *) fail "the local DELETE (a mute) is not in the session's own save: $A" ;;
esac
case "$A" in
  *"autopoints=1"*) ;;
  *) fail "the automation point is not in the session's own save: $A" ;;
esac
case "$A" in
  *"markers=[('intro', 0), ('verse', 23040000)] meter=[(0, '4/4'), (23040000, '7/8')]"*) ;;
  *) fail "the markers and the meter are not in the session's own save. 'verse' was placed at
        4 bars and a 7/8 change with it; inserting 2 bars there moved BOTH to 6 bars
        (23040000), which is the property — a marker and the meter point under it must not part
        company. Got: \$A" ;;
esac
# The removed track leaves a HOLE: ids 0, 2, 3 with 1 gone. This is the shape no authored
# fixture has.
case "$A" in
  *"ids=[0, 2, 3]"*) ;;
  *) fail "expected sparse ids [0, 2, 3] after removing the middle track: $A" ;;
esac
echo "  and it is genuinely edited: a tombstone at id 1, an override, automation, markers, a"
echo "  mid-song 7/8, and a time insert that carried both"

# ---- RELOAD IN A FRESH ENGINE AND SAVE AGAIN.
SHM2="/edrt2_$$"
start_engine "$SHM2" "$TMP/eng2.log"
cli() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load editedA --force >/dev/null 2>&1 || true
wait_load "$TMP/eng2.log" 1 "$ENG" "the edited project to reload"
sleep 1.5
cli do save editedB --force >/dev/null 2>&1 || true
sleep 1.8
# ---- NOTHING SHRANK, over the whole document rather than over a named list.
#
# The PRESENT assertions below name what these edits created, which means they cover exactly the
# things somebody thought to add. This is the net underneath them: count every array in the
# document and require that the reload lost none of them. A field nobody named is covered by the
# same rule as the ones that were, which is the difference between a check that grows one bug at a
# time and one that was already watching.
#
# COUNTED BY LEAF NAME, not by JSON path, so a schema that MOVES something is not mistaken for a
# schema that LOSES it — keyed by path, the same rule in save_roundtrip_check reported the maximal
# fixture's 158 notes as lost when they had simply moved from tracks to clips.
python3 - "$TMP/editedA.uniproj.json" "$TMP/editedB.uniproj.json" <<'PYS' || fail "the reload lost content"
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
for k in sorted(a):
    if b.get(k, 0) < a[k]:
        print(f"  FAIL: {k} {a[k]} -> {b.get(k, 0)} — the reload lost {a[k] - b.get(k, 0)}")
        ok = False
if ok:
    print("  nothing shrank across the reload (%d kinds, %d entries)" % (len(a), sum(a.values())))
raise SystemExit(0 if ok else 1)
PYS

B="$(summary "$TMP/editedB.uniproj.json")"
[ "$B" = "$A" ] || fail "reload -> save lost or changed state.
        session: $A
        reload : $B"
echo "  reload (fresh engine) -> save preserves every one of them"

# ---- AND AGAIN, for the fixed point. A field that mutates on each round trip (a normalisation
# that is not idempotent) shows up here and nowhere above.
cli do load editedB --force >/dev/null 2>&1 || true
wait_load "$TMP/eng2.log" 2 "$ENG" "the second reload"
sleep 1.5
cli do save editedC --force >/dev/null 2>&1 || true
sleep 1.8
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
C="$(summary "$TMP/editedC.uniproj.json")"
[ "$C" = "$B" ] || fail "a second round trip changed the document again, so the save is not a
        fixed point:
        first : $B
        second: $C"

# Byte-level on the whole document, not just the summarised fields — the summary cannot name a
# field nobody thought to summarise, which is exactly how the meter map went missing.
python3 - "$TMP/editedB.uniproj.json" "$TMP/editedC.uniproj.json" <<'PYC'
import json, sys
def norm(p):
    d = json.load(open(p))
    d.pop("meta", None)          # created/modified timestamps legitimately differ
    return json.dumps(d, indent=1, sort_keys=True)
b, c = norm(sys.argv[1]), norm(sys.argv[2])
if b != c:
    import difflib
    print("  FAIL: the whole document is not a fixed point under load -> save:")
    for line in list(difflib.unified_diff(b.splitlines(), c.splitlines(),
                                          "second-save", "third-save", lineterm=""))[:30]:
        print("    " + line)
    sys.exit(1)
PYC
[ $? -eq 0 ] || exit 1
echo "  and the whole document is byte-stable across a second round trip"

echo "edited_roundtrip_check: PASS — a project built by editing survives reload unchanged"
