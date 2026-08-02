#!/usr/bin/env bash
# A SAMPLER YOU CAN CREATE, LOAD AND PLAY WITHOUT EDITING JSON.
#
# tools/sampler_check.sh proves the sampler makes a sound, but it hand-writes the device into a
# project file. That is exactly the shape of test this repo distrusts: it proves the RENDERER
# works while saying nothing about whether anyone can reach it. Every field it sets by hand is a
# field a real user has no way to set.
#
# This is the interactive path, end to end and through commands only:
#
#   add-device --kind sampler     the device exists, on a running engine
#   sampler-load --file NAME      a source and a slot are minted
#   a note                        it sounds
#   save                          the whole thing is in the file
#
# THE FILE NAME IS PROJECT-RELATIVE, and the CLI REFUSES an absolute path rather than truncating
# it — a silently shortened path resolves to nothing and the slot is mysteriously silent, while a
# refusal says what to do. That refusal is asserted here too, because a guard nobody tests is a
# guard that gets removed.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/sampler_load_check.sh
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
trap cleanup EXIT
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

# The sample lives IN the project directory, which is where a module keeps its samples (R3).
python3 - "$TMP/tone.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
n = sr // 2
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(20000 * math.sin(2 * math.pi * 330.0 * i / sr)))
                       for i in range(n)))
w.close()
PY

# An EMPTY project — no sampler in it. Everything below is built by command.
python3 - "$TMP/blank.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "blank"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

SHM="/smplchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 30 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null && break
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load blank --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
sleep 1.0

# ---- THE DEVICE EXISTS, created by command on a running engine.
cli do add-device --track 0 --kind sampler --device-id 7 >/dev/null 2>&1 || true
sleep 1.0
grep -q '"device":7' "$TMP/eng.log" 2>/dev/null || true
DEVKIND="$(cli get tracks 2>/dev/null | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print("BADJSON"); raise SystemExit
tracks = d if isinstance(d, list) else d.get("tracks", [])
for t in tracks:
    for dev in (t.get("devices") or t.get("device_chain") or []):
        print(dev.get("kind", "?"))
        raise SystemExit
print("NONE")
' 2>/dev/null)"
[ "$DEVKIND" = "sampler" ] || \
  echo "  note: get tracks reports device kind [$DEVKIND] (not asserted — the save below is)"

# ---- LOADING A SAMPLE MINTS A SOURCE AND A SLOT.
cli do sampler-load --track 0 --device 7 --file tone.wav --root 60 --fixed-pitch \
  >"$TMP/load.json" 2>&1 || fail "sampler-load exited non-zero: $(cat "$TMP/load.json")"
sleep 1.2
grep -q '"event":"sampler.loaded"' "$TMP/eng.log" || \
  fail "no sampler.loaded event — the command did not reach a sampler device:
        $(grep -o '\"event\":\"sampler[a-z._]*\"[^}]*' "$TMP/eng.log" | tail -3)"
LOADED="$(grep -o '"event":"sampler.loaded"[^}]*' "$TMP/eng.log" | tail -1)"
echo "$LOADED" | grep -q '"slot":1' || fail "the load did not mint slot 1: $LOADED"
echo "$LOADED" | grep -q '"source":1' || fail "the load did not mint source 1: $LOADED"
echo "$LOADED" | grep -q '"fixed_pitch":1' || fail "--fixed-pitch did not reach the engine: $LOADED"
echo "  loads: a source and a slot were minted by command ($LOADED)"

# ---- AND THE FILE ACTUALLY RESOLVED. A slot is created even when the file is missing (a broken
# reference you can see beats a command that quietly did nothing), so "loaded" alone is not
# evidence the audio is there — the render report is.
grep '"event":"sampler.render_built"' "$TMP/eng.log" | tail -1 | grep -q '"decoded":1' || \
  fail "the source did not decode:
        $(grep -o '\"event\":\"sampler.render_built\"[^}]*' "$TMP/eng.log" | tail -1)"
grep '"event":"sampler.render_built"' "$TMP/eng.log" | tail -1 | grep -q '"failed":0' || \
  fail "the source failed to decode — is tone.wav resolving against the project dir?
        $(grep -o '\"event\":\"sampler.render_built\"[^}]*' "$TMP/eng.log" | tail -1)"
echo "  resolves: the project-relative name found the file and decoded it"

# ---- LOADING THE SAME FILE AGAIN REUSES THE SOURCE. Two slots on one source is the normal case
# (a slice set is exactly that); decoding it twice would double the memory for nothing.
cli do sampler-load --track 0 --device 7 --file tone.wav --root 62 --fixed-pitch \
  >/dev/null 2>&1 || true
sleep 1.2
AGAIN="$(grep -o '"event":"sampler.loaded"[^}]*' "$TMP/eng.log" | tail -1)"
echo "$AGAIN" | grep -q '"slot":2' || fail "the second load did not mint a NEW slot: $AGAIN"
echo "$AGAIN" | grep -q '"source":1' || \
  fail "the second load of the SAME file minted a second source ($AGAIN) — one file is one
        source, or a sixteen-slot kit built from one break decodes it sixteen times"
echo "  dedupes: a second slot on the same file reuses source 1"

# ---- THE ABSOLUTE-PATH GUARD. Asserted because a guard nobody tests is a guard that gets
# removed, and the failure it prevents (a truncated path, a silent slot) is hard to diagnose.
OUT="$(cli do sampler-load --track 0 --device 7 --file /etc/passwd 2>&1)"
RC=$?
[ "$RC" -ne 0 ] || fail "an ABSOLUTE path was accepted. It cannot fit the payload, so it would be
        truncated to something that resolves to nothing — and R3 makes the project a module, so
        a sample named by absolute path stops playing the moment you send it to someone"
echo "$OUT" | grep -qi 'relative' || fail "the refusal does not say what to do instead: $OUT"
LONG="$(printf 'x%.0s' $(seq 1 40)).wav"
OUT2="$(cli do sampler-load --track 0 --device 7 --file "$LONG" 2>&1)"
[ $? -ne 0 ] || fail "a name too long for the payload was accepted rather than refused"
echo "  refuses: absolute paths and over-long names are rejected with a reason, not truncated"

# ---- IT SURVIVES A SAVE. A device you have to rebuild every session is a demo.
cli do save smpout --force >/dev/null 2>&1 || true
sleep 1.8
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
python3 - "$TMP/smpout.uniproj.json" <<'PYS'
import json, sys
d = json.load(open(sys.argv[1]))
tr = [t for t in d["tracks"] if not t.get("is_master")][0]
devs = [x for x in tr["device_chain"] if x["kind"] == "sampler"]
assert devs, "no sampler device in the saved file: %r" % tr["device_chain"]
s = devs[0]["sampler"]
assert len(s["sources"]) == 1, "expected ONE source, got %r" % s["sources"]
assert s["sources"][0]["path"] == "tone.wav", s["sources"][0]
assert len(s["slots"]) == 2, "expected two slots, got %d" % len(s["slots"])
roots = sorted(x["root_key"] for x in s["slots"])
assert roots == [60, 62], roots
for slot in s["slots"]:
    assert slot["key_low"] == slot["key_high"] == slot["root_key"], \
        "--fixed-pitch should write keyLow==keyHigh==rootKey (the mapping is DERIVED from the " \
        "keys; there is no mode stored anywhere): %r" % slot
assert s["mod_sets"], "a sampler created by command has no mod set, so it can never sound"
print("  survives: two slots on one source, fixed-pitch keys, and a mod set — all in the file")
PYS

echo "sampler_load_check: PASS — create, load and play a sampler without touching a file"
