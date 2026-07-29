#!/usr/bin/env bash
# Proves a legacy (schema<=3) track-level patcher migrates into a head-of-chain
# PatcherEvent DEVICE that ACTUALLY RUNS: the migrated generator drives the
# instrument and produces audio. The track has NO clips and NO placements, so any
# sound is 100% the generator — if the migration were dropping or mis-placing the
# graph, this capture would be silent.
#
# The structural half (a head PatcherEvent device with the right ids) is covered
# deterministically by project_file_tests (ctest project_file_round_trip). This is
# the end-to-end audio half; it needs a real audio device (non-test mode) + Zebra2.
#
#   tools/patcher_device_migration_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"
TAKE="$TMP/gen.wav"
SHM="/patmig_$$"
ZEBRA="/Library/Audio/Plug-Ins/VST3/Zebra2.vst3"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }
[ -d "$ZEBRA" ] || { echo "SKIP: Zebra2 not installed at $ZEBRA"; exit 0; }

# A legacy schema-3 project: ONE track carrying a track-level euclidean ->
# random_degree -> event_out generator and a Zebra2 instrument. No placements, no
# clips: the only possible sound is the generator's.
cat > "$TMP/gentrack.uniproj.json" <<EOF
{ "schema_version": 3,
  "meta": { "name": "gentrack", "created_utc": 0, "modified_utc": 0 },
  "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
  "harmony_timeline": [],
  "clips": [],
  "tracks": [ { "track_id": 0, "name": "Gen", "harmony_quantize": 0, "lines_per_beat": 4,
    "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
    "device_chain": [ { "device_id": 0, "kind": "vst_instrument", "capability_mask": 5,
      "patcher_node_id": 0, "host_slot_index": 4294967294, "bypass": false,
      "vst_ref": { "vendor": "u-he", "name": "Zebra2", "path": "$ZEBRA", "uid16": "" } } ],
    "mod_links": [], "placements": [],
    "patcher": {
      "nodes": [
        { "id": 0, "type": "euclidean", "euclidean": { "steps": 16, "hits": 5, "offset": 0,
          "duration_ticks": 0, "degree": 1, "octave_offset": 0, "velocity": 100, "base_octave": 4 } },
        { "id": 1, "type": "random_degree", "random_degree": { "degree": 8, "velocity": 100, "duration_ticks": 0 } },
        { "id": 2, "type": "event_out" } ],
      "edges": [
        { "src_node_id": 0, "src_port_id": 1, "dst_node_id": 1, "dst_port_id": 0, "kind": "event" },
        { "src_node_id": 1, "src_port_id": 1, "dst_node_id": 2, "dst_port_id": 0, "kind": "event" } ] } } ] }
EOF

( cd "$BUILD" && DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    DAW_CAPTURE_WAV="$TAKE" DAW_CAPTURE_SECONDS=6 \
    ./daw_engine --run-seconds 7 >"$TMP/engine.log" 2>&1 ) &
ENGINE=$!
sleep 2.5
DAW_UI_SHM_NAME="$SHM" "$CLI" do load gentrack --force >/dev/null 2>&1 || true
sleep 1
DAW_UI_SHM_NAME="$SHM" "$CLI" do play --force >/dev/null 2>&1 || true
wait "$ENGINE" 2>/dev/null || true

ok=1
# The migrated generator must load as an executable patcher graph. A single
# generator device takes the single-graph path (patcher_loaded); two or more take
# patcher_assembled. Accept either.
if grep -qE "project\.patcher_(loaded|assembled)" "$TMP/engine.log"; then
  echo "PASS: migrated track-level patcher runs as a head PatcherEvent device"
else
  echo "FAIL: no patcher graph loaded/assembled — migration did not produce an executable device"
  grep -iE "patcher|migrat|error|fail" "$TMP/engine.log" | tail -10
  ok=0
fi

echo "--- perceptual (generator-only track; silence => migration broke generation) ---"
if python3 "$ROOT/tools/perceptual.py" --expect-audio "$TAKE"; then
  echo "PASS: generator produced audio through the instrument"
else
  echo "FAIL: generator-only track was silent"
  ok=0
fi

rm -rf "$TMP"
[ "$ok" = "1" ] && echo "patcher_device_migration_check: PASS" \
                || { echo "patcher_device_migration_check: FAIL"; exit 1; }
