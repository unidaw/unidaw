#!/usr/bin/env bash
# Proves a patcher that lives on a DEVICE actually runs. Two scenarios, both with a
# generator-only track (no clips, no placements), so any sound is 100% the patcher —
# silence would mean the graph did not execute.
#
#   A. A legacy (schema<=3) track-level patcher migrates into a head-of-chain
#      patcher_event DEVICE with a resolved event_out node id, and drives Zebra2.
#   B. An explicit patcher_event device whose patcher_node_id is the 0xFFFFFFFF
#      "natural output" SENTINEL — the exact case that used to run silent because
#      the per-track node filter could not seed from a non-existent node. The load
#      path now resolves the sentinel to the real event_out, so it sounds.
#
# The structural half (a head patcher_event device with the right ids) is covered
# deterministically by project_file_tests (ctest project_file_round_trip). This is
# the end-to-end audio half; it needs a real audio device (non-test mode) + Zebra2.
#
#   tools/patcher_device_migration_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
# Prefer a debug build, fall back to release, so a release-only tree still works.
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
ZEBRA="/Library/Audio/Plug-Ins/VST3/Zebra2.vst3"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }
[ -d "$ZEBRA" ] || { echo "SKIP: Zebra2 not installed at $ZEBRA"; exit 0; }

# The euclidean -> random_degree -> event_out generator, as a schema-2 patcher graph.
GRAPH='"patcher": { "nodes": [
    { "id": 0, "type": "euclidean", "euclidean": { "steps": 16, "hits": 5, "offset": 0, "duration_ticks": 0, "degree": 1, "octave_offset": 0, "velocity": 100, "base_octave": 4 } },
    { "id": 1, "type": "random_degree", "random_degree": { "degree": 8, "velocity": 100, "duration_ticks": 0 } },
    { "id": 2, "type": "event_out" } ],
  "edges": [ { "src_node_id": 0, "src_port_id": 1, "dst_node_id": 1, "dst_port_id": 0, "kind": "event" },
             { "src_node_id": 1, "src_port_id": 1, "dst_node_id": 2, "dst_port_id": 0, "kind": "event" } ] }'
ZDEV='{ "device_id": 9, "kind": "vst_instrument", "capability_mask": 5, "patcher_node_id": 0, "host_slot_index": 4294967294, "bypass": false, "vst_ref": { "vendor": "u-he", "name": "Zebra2", "path": "'"$ZEBRA"'", "uid16": "" } }'

# scenario <name> <fixture-json>: load through the engine, capture, assert audio.
# The window is deliberately generous — load at 2.5s, play at 3.5s, capture 12s of a
# 14s run — so ~8s of playback lands in the WAV even on a busy machine. A tight window
# made the audio assertion flaky (the graph-assembly half was always fine), so give it
# room rather than race it.
scenario() {
  local name="$1" json="$2"
  local tmp take shm
  tmp="$(mktemp -d)"; take="$tmp/$name.wav"; shm="/patmig_${name}_$$"
  printf '%s\n' "$json" > "$tmp/$name.uniproj.json"
  # RENDERED OFFLINE, which also RETIRES a workaround. This used to run at 8 pipeline blocks
  # instead of the 3-block default because a starved producer emits silence that looks exactly
  # like a broken generator — and this check is about whether the graph GENERATES, so scheduling
  # had to be bought out of the way. The render pump never starves: it WAITS rather than
  # emitting silence to stay current, so there is no depth to tune and no starvation to mistake
  # for a dead generator.
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$tmp" \
      ./daw_engine --project "$name" --render "$(basename " --sample-rate 44100$take" .wav)" --run-seconds 12 \
      >"$tmp/engine.log" 2>&1 ) \
    || { echo "  FAIL: the '$name' render exited non-zero — see $tmp/engine.log"; return 1; }
  local ok=1
  # Two independent halves, reported distinctly so a failure says WHICH broke: the
  # engine never running the graph (a real migration/setup bug) is not the same as the
  # graph running but the capture being empty (a timing/generation issue).
  if ! grep -qE "project\.patcher_(loaded|assembled)" "$tmp/engine.log"; then
    echo "  [$name] FAIL (setup): engine never loaded/assembled the patcher graph"
    ok=0
  fi
  if python3 "$ROOT/tools/perceptual.py" --expect-audio "$take" >/dev/null 2>&1; then
    echo "  [$name] PASS: patcher device produced audio"
  elif [ "$ok" = "1" ]; then
    # Distinguish "the generator produced nothing" from "the machine never let the
    # producer run". A starved pipeline outputs silence for reasons that have nothing
    # to do with the patcher, and reporting that as a generation failure sends the
    # next reader hunting through the patcher code for a bug that is not there.
    starved="$(sed -n 's/.*summary: \([0-9]*\) of \([0-9]*\) playback callbacks.*/\1 \2/p' \
                 "$tmp/engine.log" | tail -1)"
    dropped="${starved%% *}"; total="${starved##* }"
    if [ -n "$dropped" ] && [ -n "$total" ] && [ "$total" -gt 0 ] \
       && [ $((dropped * 2)) -gt "$total" ]; then
      echo "  [$name] INCONCLUSIVE: the audio pipeline starved ($dropped of $total callbacks"
      echo "        dropped a track), so the silence says nothing about the patcher. Raise"
      echo "        DAW_ENGINE_NUM_BLOCKS or use a wired output device and re-run."
      ok=0
    else
      echo "  [$name] FAIL (audio): graph ran, pipeline was fed, and the capture was still silent"
      ok=0
    fi
  fi
  rm -rf "$tmp"
  [ "$ok" = "1" ]
}

hdr='"schema_version": %d, "meta": { "name": "%s" }, "nanoticks_per_quarter": '"$Q"', "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [], "clips": []'

# A: legacy track-level patcher (schema 3) -> migrates to a head patcher_event device.
A=$(printf '{ %s, "tracks": [ { "track_id": 0, "name": "Gen", "device_chain": [ %s ], "placements": [], %s } ] }' \
    "$(printf "$hdr" 3 migrate)" "$ZDEV" "$GRAPH")

# B: explicit patcher_event device with the 0xFFFFFFFF natural-output SENTINEL.
B=$(printf '{ %s, "tracks": [ { "track_id": 0, "name": "Gen", "device_chain": [ { "device_id": 0, "kind": "patcher_event", "capability_mask": 3, "patcher_node_id": 4294967295, "host_slot_index": 0, "bypass": false, %s }, %s ], "placements": [] } ] }' \
    "$(printf "$hdr" 4 sentinel)" "$GRAPH" "$ZDEV")

ok=1
echo "A. legacy track-level patcher -> head device:"
scenario migrate "$A" || ok=0
echo "B. patcher_event device with 0xFFFFFFFF sentinel node id:"
scenario sentinel "$B" || ok=0

[ "$ok" = "1" ] && echo "patcher_device_migration_check: PASS" \
                || { echo "patcher_device_migration_check: FAIL"; exit 1; }
