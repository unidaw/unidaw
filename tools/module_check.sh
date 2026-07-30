#!/usr/bin/env bash
# THE `.uni` MODULE: ONE FILE YOU CAN SEND SOMEONE.
#
# "Live songs and Renoise songs ARE zips. It's easy to send someone the zip, named .uni. Mods,
# xm's all do this." — the owner, ruling R3 in docs/SAMPLER_DESIGN.md.
#
# THE ASSERTION THAT MATTERS IS THE MOVE. A save that packs samples and then loads them from the
# original directory proves nothing at all: the paths still resolve, so the module could be empty
# and everything would still play. So this check DELETES the originals and unpacks somewhere else
# entirely, which is what "sending someone the zip" actually means.
#
# FIVE PROPERTIES:
#   PACKS       the samples are INSIDE the file, and its paths point inside it
#   REAL ZIP    `unzip -t` accepts it — a file only this reader can open is not a module
#   TRAVELS     unpacked in a DIFFERENT directory with the originals GONE, it still plays
#   REPRODUCIBLE saving twice with no edits produces a byte-identical file
#   REFUSES     a corrupt module is refused rather than opened with a sample missing
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/module_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

HOME_DIR="$(mktemp -d)"   # where the song is made
AWAY_DIR="$(mktemp -d)"   # the other machine
trap 'rm -rf "$HOME_DIR" "$AWAY_DIR"' EXIT
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

python3 - "$HOME_DIR/tone.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
n = sr // 2
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(19000 * math.sin(2 * math.pi * 440.0 * i / sr)))
                       for i in range(n)))
w.close()
PY

python3 - "$HOME_DIR/blank.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
BAR = Q * 4
# A note at bar 1, so "it still plays" is measurable after the move.
clip = {"id": 1, "name": "p", "length": BAR * 2, "kind": "symbolic",
        "notes": [{"nanotick": BAR, "duration": Q, "pitch": 60, "velocity": 120,
                   "column": 0, "note_id": 1}]}
tr = {"track_id": 0, "name": "M", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 2,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "blank"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

SHM="/modchk_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$HOME_DIR" \
    ./daw_engine --run-seconds 40 >"$HOME_DIR/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$HOME_DIR/eng.log" 2>/dev/null && break
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$HOME_DIR" "$CLI" "$@"; }
cli do load blank --force >/dev/null 2>&1 || true
for _ in $(seq 1 80); do
  grep -q '"event":"project.load"' "$HOME_DIR/eng.log" 2>/dev/null && break
  sleep 0.25
done
sleep 1.0
cli do add-device --track 0 --kind sampler --device-id 1 >/dev/null 2>&1 || true
sleep 1.0
cli do sampler-load --track 0 --device 1 --file tone.wav --root 60 --fixed-pitch >/dev/null 2>&1 || true
sleep 1.5
grep -q '"event":"sampler.loaded"' "$HOME_DIR/eng.log" || fail "the sample did not load"

# ---- PACKS.
cli do save-module song >/dev/null 2>&1 || true
sleep 2.5
grep -q '"event":"project.module_saved"' "$HOME_DIR/eng.log" || \
  fail "no project.module_saved event: $(grep -o '\"event\":\"project.module[a-z_]*\"[^}]*' "$HOME_DIR/eng.log" | tail -2)"
grep '"event":"project.module_saved"' "$HOME_DIR/eng.log" | tail -1 | grep -q '"ok":true' || \
  fail "the module save reported failure:
        $(grep -o '\"event\":\"project.module_saved\"[^}]*' "$HOME_DIR/eng.log" | tail -1)"
[ -s "$HOME_DIR/song.uni" ] || fail "no song.uni was written"
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
echo "  packs: song.uni written ($(wc -c < "$HOME_DIR/song.uni" | tr -d ' ') bytes)"

# ---- REAL ZIP. A file only this reader can open is not a module — you would not find out until
# you had sent it to somebody.
if command -v unzip >/dev/null 2>&1; then
  unzip -t "$HOME_DIR/song.uni" >/dev/null 2>&1 || \
    fail "the system unzip REJECTS song.uni. A container only our own reader accepts is not a
        module, and the failure would surface at the worst possible moment — after sending it"
  unzip -l "$HOME_DIR/song.uni" 2>/dev/null | grep -q 'samples/tone.wav' || \
    fail "the module does not contain samples/tone.wav — the sample is not IN the file"
  unzip -l "$HOME_DIR/song.uni" 2>/dev/null | grep -q 'project.json' || \
    fail "the module does not contain project.json"
  echo "  real zip: the system unzip accepts it, and it holds project.json + samples/tone.wav"
else
  echo "  note: unzip not present, skipping the third-party reader check"
fi

# ---- REPRODUCIBLE. Saving twice with no edits must produce the same bytes, or every save is a
# diff and "did anything actually change?" stops being answerable.
( cd "$BUILD" && env DAW_UI_SHM_NAME="/modchk2_$$" DAW_PROJECT_DIR="$HOME_DIR" \
    ./daw_engine --run-seconds 20 >"$HOME_DIR/eng2.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$HOME_DIR/eng2.log" 2>/dev/null && break
  sleep 0.25
done
cli2() { DAW_UI_SHM_NAME="/modchk2_$$" DAW_PROJECT_DIR="$HOME_DIR" "$CLI" "$@"; }
cli2 do load song --force >/dev/null 2>&1 || true
sleep 2.0
# THE SAME NAME TWICE, not two names. The project's NAME is part of the document, so `song2` and
# `song3` are legitimately different files — comparing those would be testing that the name is
# stored, which it is. Copy the first save aside and overwrite it with a second.
cli2 do save-module song2 >/dev/null 2>&1 || true
sleep 2.5
cp "$HOME_DIR/song2.uni" "$HOME_DIR/song2.first.uni" 2>/dev/null || true
cli2 do save-module song2 >/dev/null 2>&1 || true
sleep 2.5
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
if [ -s "$HOME_DIR/song2.first.uni" ] && [ -s "$HOME_DIR/song2.uni" ]; then
  cmp -s "$HOME_DIR/song2.first.uni" "$HOME_DIR/song2.uni" || \
    fail "two saves of the same project produced DIFFERENT bytes. The archive timestamp is fixed
        at the format's epoch precisely so this holds — without it every save shows as a change
        in git and nobody can tell whether anything really did"
  echo "  reproducible: two saves of one project are byte-identical"
else
  fail "the reproducibility saves did not both write"
fi

# ---- TRAVELS. THE ASSERTION THAT MATTERS. Move ONLY the .uni to another directory, DELETE the
# originals, and open it there. If the module were empty, or its paths still pointed at the old
# location, this is where it falls over — and nowhere earlier.
cp "$HOME_DIR/song.uni" "$AWAY_DIR/song.uni"
rm -f "$HOME_DIR/tone.wav" "$HOME_DIR"/*.uniproj.json
SHM3="/modchk3_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM3" DAW_PROJECT_DIR="$AWAY_DIR" \
    ./daw_engine --run-seconds 25 >"$AWAY_DIR/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$AWAY_DIR/eng.log" 2>/dev/null && break
  sleep 0.25
done
cli3() { DAW_UI_SHM_NAME="$SHM3" DAW_PROJECT_DIR="$AWAY_DIR" "$CLI" "$@"; }
cli3 do load-module song >/dev/null 2>&1 || true
sleep 3.0
grep '"event":"project.module_loaded"' "$AWAY_DIR/eng.log" | tail -1 | grep -q '"ok":true' || \
  fail "the module did not load on the other machine:
        $(grep -o '\"event\":\"project.module_loaded\"[^}]*' "$AWAY_DIR/eng.log" | tail -1)"
# AND ITS SAMPLE RESOLVED. Loading is not enough — a module whose paths still pointed at the old
# machine would load perfectly and be silent.
grep '"event":"sampler.render_built"' "$AWAY_DIR/eng.log" | tail -1 | grep -q '"decoded":1' || \
  fail "the module loaded but its sample did NOT decode on the other machine:
        $(grep -o '\"event\":\"sampler.render_built\"[^}]*' "$AWAY_DIR/eng.log" | tail -1)"
grep '"event":"sampler.render_built"' "$AWAY_DIR/eng.log" | tail -1 | grep -q '"failed":0' || \
  fail "a source failed to resolve after the move — the module is referring to files that
        travelled with nothing:
        $(grep -o '\"event\":\"sampler.render_built\"[^}]*' "$AWAY_DIR/eng.log" | tail -1)"
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
echo "  travels: unpacked in a different directory with the originals DELETED, the sample resolved"

# ---- REFUSES a corrupt module. Half a download is the usual way a module arrives broken, and
# opening it with one sample missing is only discovered when that pad is played.
python3 - "$AWAY_DIR/song.uni" "$AWAY_DIR/bad.uni" <<'PYC'
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
# Flip a byte well inside the first entry's payload; the CRC no longer matches.
b[200] ^= 0xFF
open(sys.argv[2], 'wb').write(bytes(b))
PYC
SHM4="/modchk4_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM4" DAW_PROJECT_DIR="$AWAY_DIR" \
    ./daw_engine --run-seconds 15 >"$AWAY_DIR/eng2.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$AWAY_DIR/eng2.log" 2>/dev/null && break
  sleep 0.25
done
DAW_UI_SHM_NAME="$SHM4" DAW_PROJECT_DIR="$AWAY_DIR" "$CLI" do load-module bad >/dev/null 2>&1 || true
sleep 2.0
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
BAD="$(grep -o '"event":"project.module_loaded"[^}]*' "$AWAY_DIR/eng2.log" | tail -1)"
echo "$BAD" | grep -q '"ok":false' || \
  fail "a CORRUPT module loaded successfully ($BAD). It must be refused: a module that opens
        with a sample quietly missing turns 'my song sounds wrong' into an investigation"
echo "  refuses: a corrupt module is rejected with a reason, not opened"

echo "module_check: PASS — one file, samples inside, and it plays on the other machine"
