#!/usr/bin/env bash
# ZERO IS NOT A DEVICE ID. IT IS THE ABSENCE OF ONE.
#
# The chain-local allocator started at 0, so the first device added to an empty chain got id 0 —
# and 0 is what "there is no device" means everywhere else in the engine. (That allocator is gone:
# ids come from the project's next_device_id watermark now, and `addDevice` refuses anything
# outside [1, 0x7FFF] — AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME. This check is what says the
# property survived the move, which is the only reason it is still worth running.) TrackRuntime::samplerDeviceId is
# documented "0 = this track has no sampler" and guarded that way at nine sites, so a sampler
# that was the FIRST device on its track was never sent a note: every guard read "no sampler
# here". The wire protocol overloads it identically — deviceId 0 on a command means "the first
# sampler on this track, whichever that is" — so such a device was unaddressable by every command
# as well.
#
# That is the ordinary case. `add-device --kind sampler` on a fresh track produces exactly it,
# and so does the whole chop workflow.
#
# WHY NOTHING HERE CAUGHT IT, which is the part worth keeping. Every structural fact stayed
# correct: sampler.loaded fired, the slots resolved with no source-missing flag, the notes were
# emitted at the right ticks, unmapped stayed 0, and the kit read-back was perfect throughout.
# The instrument was simply never played. The web-UI agent found it from outside with a
# three-track differential — empty chain, patcher first, plugin first — where the only track that
# went silent was the one whose sampler landed at id 0.
#
# A read-back check cannot find this, because the read-back is right. Only the AUDIO is wrong.
#
# TWO PATHS, because they mint ids differently and only one of them is a file:
#   LOADED    a project that says device_id 0 is REPAIRED at load and plays
#   ADDED     a sampler added by command to an empty chain gets a non-zero id
#
#   tools/device_id_zero_check.sh
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
. "$ROOT/tools/lib/engine_wait.sh"

python3 - "$TMP/s.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
n = sr
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(
    struct.pack('<h', int(14000 * math.sin(2 * math.pi * 300.0 * i / sr) * max(0.0, 1 - i / 24000)))
    for i in range(n)))
w.close()
PY

# project <name> <deviceId>
project() {
  python3 - "$TMP/$1.uniproj.json" "$Q" "$2" <<'PY'
import json, sys
out, Q, devid = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 1000, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
# THE SAMPLER IS THE ONLY DEVICE and it carries the id under test. No plugin in front of it,
# because a plugin in front is precisely what used to push the sampler off id 0 and hide this.
dev = {"device_id": devid, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "s.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
notes = [{"nanotick": i * Q, "duration": Q // 2, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": i + 1} for i in range(4)]
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "d"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY
}

peak() {  # peak <renderName>
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n = w.getnchannels(), w.getnframes()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
print(max((abs(v) for v in s), default=0))
PY
}

render() {  # render <project> <name>
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/devid_${$}_$2" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --project "$1" --render "$2" --sample-rate 44100 --run-seconds 5 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

# ---- THE CONTROL FIRST. An explicit id 1 must play, or a silent id 0 below proves nothing
# about ids and everything about the fixture.
project one 1
render one one
P1="$(peak one)"
[ "${P1:-0}" -gt 1000 ] || \
  fail "the sampler with device_id 1 is silent (peak ${P1:-0}), so this fixture cannot make sound
        at all and the id comparison below would be meaningless"
echo "  device_id 1: peak $P1"

# ---- LOADED. A project that says 0 must be repaired and must sound the same.
project zero 0
render zero zero
P0="$(peak zero)"
echo "  device_id 0: peak $P0"
[ "${P0:-0}" -gt 1000 ] || \
  fail "a sampler saved with device_id 0 is SILENT (peak ${P0:-0}) where the same project with
        device_id 1 gives $P1. Zero is the engine's 'this track has no sampler' sentinel, so
        every guard skips a device that genuinely has that id — and the read-back is perfect
        throughout, which is why only audio can catch it"

# ---- ADDED. The command path mints its own ids, and an empty chain is where it used to mint 0.
export DAW_UI_SHM_NAME="/devid_add_$$" DAW_PROJECT_DIR="$TMP"
python3 - "$TMP/empty.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "e"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY
( cd "$BUILD" && exec ./daw_engine --project empty --run-seconds 25 >"$TMP/add.log" 2>&1 ) &
ENG=$!
# project.load, not "starting threads": that line is printed BEFORE the startup project is
# loaded, and a command sent on it reaches an engine whose tracks do not exist yet.
wait_for_boot "$TMP/add.log" "$ENG" 160
grep -q '"event":"project.load"' "$TMP/add.log" 2>/dev/null || fail "the engine never loaded"

"$CLI" do add-device --track 0 --kind sampler >/dev/null 2>&1
ADDED=""
for _ in $(seq 1 40); do
  ADDED="$("$CLI" get sampler-kit --track 0 2>/dev/null |
           grep -oE '"device": [0-9]+' | head -1 | grep -oE '[0-9]+$')"
  [ -n "${ADDED:-}" ] && break
  sleep 0.25
done
[ -n "${ADDED:-}" ] || fail "the added sampler never appeared in the kit read-back"
echo "  added to an empty chain: device id $ADDED"
[ "$ADDED" != "0" ] || \
  fail "a sampler added to an EMPTY chain was given device id 0 — the same value that means
        'this track has no sampler'. An allocated id must be >= 1: the guards cannot tell a
        real id 0 from an absent one, and this is the ordinary way a sampler gets made"

echo "device_id_zero_check: PASS — zero is reserved, and a first-device sampler plays"
