#!/usr/bin/env bash
# THE SAMPLER'S VOICE TELEMETRY IS REAL — active voices, steals, unmapped, and the voice cap.
#
# All four are published in UiSamplerKitRegion and readable with `get sampler-kit`, and until this
# file NOTHING ASSERTED ANY OF THEM. That is how they were found: making `activeVoices()` read a
# published counter instead of walking the voice pool (it was walking it from the COMMAND thread
# while the producer mutated those voices) could have left the number stuck at zero forever, and
# no check in the suite would have noticed. A number nobody asserts is a number that can quietly
# become a constant.
#
# WHAT PROVES THE POOL HANDOFF, corrected after running the control rather than reasoning about
# it: configure() no longer resizes the voice pool on the command thread — it PUBLISHES a pending
# cap that the producer adopts at a block boundary. My first draft claimed `voice_cap` proved that
# handoff. It does not. `voice_cap` is published from `snap->state.voiceCap`, i.e. from the
# DOCUMENT, so it reads back 16 whether or not the producer ever sized the pool — and the negative
# control confirmed it: with adoption disabled, ADOPTED still passed and SOUNDING is what caught
# it. The property that proves the handoff is that voices actually sound, because an unadopted
# pool is an EMPTY one.
#
# NO AUDIO DEVICE NEEDED, and that is worth saying because it looks like it should. The producer
# runs whether or not the output device ever asks for a block, and this reads the published region
# rather than a capture — so it works on a machine whose coreaudiod is dead.
#
# FIVE PROPERTIES:
#   ROUNDTRIP voice_cap reads back the project's 16, not the default 64 — the document's setting
#             survives into the published region. This does NOT prove the pool was sized; see above
#   QUIET     active_voices is 0 before the transport rolls
#   SOUNDING  active_voices goes ABOVE ZERO while notes are held, and never exceeds voice_cap.
#             This is also the only property that detects a pool the producer never adopted
#   MAPPED    unmapped stays 0 for a kit whose slot covers the pitches played — an unmapped count
#             that climbs means notes are being dropped before they reach a voice
#   RELEASED  active_voices falls back to 0 once the notes are over, so the count is being
#             refreshed rather than latched at its high-water mark
#
#   tools/voice_telemetry_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
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
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }
say() { [ "$(eval "echo \$$1")" = "1" ] && echo "  $2"; return 0; }

python3 - "$TMP/s.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * 220.0 * i / sr)))
                       for i in range(sr * 3)))
w.close()
PY

# EIGHT OVERLAPPING NOTES, each two beats long and starting an eighth apart, so several are
# sounding at once for most of the bar. One note at a time would make "above zero" true of a
# sampler that could only ever play one voice, which is not the property.
#
# voice_cap 16, not the default 64: a cap that equals the default cannot tell an adopted setting
# from an untouched one.
python3 - "$TMP/vt.uniproj.json" "$TMP/s.wav" <<'PY'
import json, sys
Q = 960000; BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 1,
       "patcher_node_id": 4294967295, "host_slot_index": 4294967294, "bypass": False,
       "sampler": {"voice_cap": 16,
                   "sources": [{"local_id": 1, "path": sys.argv[2]}],
                   "slots": [{"id": 1, "source_local_id": 1, "root_key": 60,
                              "low_key": 0, "high_key": 127, "gate": 1}]}}
notes = [{"nanotick": i * Q // 2, "duration": Q * 2, "pitch": 60 + i,
          "velocity": 100, "column": i % 4, "note_id": i + 1} for i in range(8)]
clip = {"id": 1, "name": "c", "length": BAR * 2, "kind": "symbolic",
        "lines_per_beat": 4, "notes": notes}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 2,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "vt"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(sys.argv[1], "w"))
PY

SHM="/vtel_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project vt --run-seconds 60 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

kitfield() {  # kitfield <name>
  cli get sampler-kit --track 0 2>/dev/null |
    sed -n "s/.*\"$1\": \([0-9]*\).*/\1/p" | head -1
}

ok=1
wait_for_published 30 "16" kitfield voice_cap || true
CAP="$(kitfield voice_cap)"
[ "${CAP:-0}" = "16" ] || \
  fail "voice_cap reads '${CAP:-missing}', not the project's 16 — the document's setting is not
        reaching the published region, so the fixture is not what this check thinks it is"
echo "  roundtrip: voice_cap is 16, the document's setting (this says nothing about the pool)"

Q0="$(kitfield active_voices)"
[ "${Q0:-0}" = "0" ] || \
  { echo "  FAIL: active_voices is ${Q0} before the transport rolled"; ok=0; }
say ok "quiet: no voices before play"

cli do play --force >/dev/null 2>&1
PEAKV=0
UNMAPPED=0
for _ in $(seq 1 24); do
  sleep 0.25
  V="$(kitfield active_voices)"
  U="$(kitfield unmapped)"
  [ -n "${V:-}" ] && [ "${V:-0}" -gt "$PEAKV" ] && PEAKV="$V"
  [ -n "${U:-}" ] && [ "${U:-0}" -gt "$UNMAPPED" ] && UNMAPPED="$U"
done

soundok=1
[ "$PEAKV" -gt 0 ] || \
  { echo "  FAIL: active_voices never rose above 0 while eight overlapping notes were held. TWO
        causes and the log tells you which: the producer publishes this count once per block, so
        either the counter is stuck, OR the voice pool was never sized because configure()'s
        pending cap was not adopted — an unadopted pool is an empty one and nothing can sound.
        Check whether the render is silent too: that separates them"; ok=0; soundok=0; }
[ "$PEAKV" -le 16 ] || \
  { echo "  FAIL: active_voices reached $PEAKV, above the cap of 16 — the pool is not bounding
        itself, which is the setting's entire job"; ok=0; soundok=0; }
say soundok "sounding: active_voices peaked at $PEAKV, within the cap of 16"

[ "$UNMAPPED" = "0" ] || \
  { echo "  FAIL: unmapped climbed to $UNMAPPED on a kit whose slot spans every key played —
        notes are being dropped before they reach a voice"; ok=0; }
say ok "mapped: unmapped stayed 0"

# RELEASED. The notes run out well inside the fixture, so a count that is refreshed per block
# falls back; one latched at its high-water mark does not.
cli do stop --force >/dev/null 2>&1
relok=0
for _ in $(seq 1 40); do
  sleep 0.25
  [ "$(kitfield active_voices)" = "0" ] && { relok=1; break; }
done
[ "$relok" = "1" ] || \
  { echo "  FAIL: active_voices never returned to 0 after the transport stopped (last read
        '$(kitfield active_voices)') — the number is latched rather than refreshed"; ok=0; }
say relok "released: the count fell back to 0 after stop"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ "$ok" = "1" ] && echo "voice_telemetry_check: PASS — the voice counters are published, bounded and refreshed" \
                || { echo "voice_telemetry_check: FAIL"; exit 1; }
