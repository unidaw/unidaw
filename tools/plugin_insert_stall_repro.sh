#!/usr/bin/env bash
# INSERTING A PLUGIN WHILE THE TRANSPORT RUNS MUST NOT SILENCE THE SONG.
#
# READ THIS FIRST: IT DOES NOT REPRODUCE THE BUG, AND IT IS NOT REGISTERED IN ctest.
#
# It was written to be the live half of the completedMinimum fix. It is not, and the negative
# control is how I know: with the rule disabled — and again with DAW_ENGINE_NUM_BLOCKS=2, which
# needs a skip of only two blocks to trip the gate — it still reported ZERO inFlight stalls after
# the insert and passed. Green with the fix reverted means it guards nothing.
#
# WHY. The deadlock needs a track's controllerMutex held across more than numBlocks block
# periods (~23 ms at 512/44100 with numBlocks 2). The in-tree Identity plugin instantiates far
# faster than that, so the dispatch is never actually skipped. The six-Zebra2 project where this
# was first measured loads for 4-7 blocks per plugin; Identity does not come close.
#
# WHAT A REAL LIVE REPRODUCTION WOULD NEED: a plugin whose instantiation is genuinely slow, or a
# test-only hook that holds runtime.controllerMutex for a set number of blocks so the skip is
# deterministic and needs no third-party plugin at all. The second is the better engineering —
# it puts the capability where the product can use it — and is what I would build with more time
# than a demo week allows.
#
# THE RULE IS PINNED ELSEWHERE, properly: testCompletedMinimum in
# apps/engine_rt_helpers_tests_main.cpp states the measured frozen state as an assertion, and its
# control fails exactly those lines. Keep this script for the day someone has a slow plugin to
# point it at; do not read its PASS as evidence of anything.
#
# THE BUG IT WAS MEANT TO PIN. Loading a VST holds that track's controllerMutex for the whole
# instantiation — apps/engine_chain_host.cpp takes the lock and then makes a BLOCKING round-trip
# the host cannot answer until it has finished loading the plugin. For those blocks
# apps/engine_produce_block.cpp try_locks the same mutex, fails, and returns, so the track is
# never SENT them. When the lock releases the track rejoins the back-pressure minimum carrying a
# STALE block id, further behind than numBlocks; `inFlight >= numBlocks` then makes the producer
# `continue` past produceBlock, dispatching to NOBODY. The lagging host can never advance,
# because the blocks it is missing are exactly the ones the closed gate prevents being sent.
# Measured, before the fix:
#   next=54 minCompleted=49 playback=1638 hosts=[0:49,1:53,2:53,3:53,4:53,5:53]
# — the audio callback 1732 blocks along, and twenty seconds of digital silence.
#
# WHY THE ASSERTION IS THE STALL COUNT AND NOT AN RMS WINDOW. Three checks in this tree have now
# measured the wrong stretch of audio by taking absolute offsets into a capture whose origin is
# when the DEVICE started, not when the interesting thing happened; one of them passed with the
# fix reverted, which is the only reason the windows were known to be meaningless. What the fix
# changes is whether the producer stays gated, and the producer says so in its own words. A
# marker written INTO the engine log makes "after the insert" a position in that same stream
# rather than a wall-clock guess.
#
# The deadlock is permanent when it happens, so a handful of stalls is scheduling and a hundred
# is the bug. The threshold is deliberately loose for that reason.
#
#   tools/plugin_insert_stall_repro.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
IDENTITY="$BUILD/identity_plugin_artefacts/VST3/Identity.vst3"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }
# Identity is built IN THIS TREE, so this check needs nothing installed on the machine — which is
# the whole reason it can be registered rather than living in a scratch directory.
[ -d "$IDENTITY" ] || { echo "SKIP: build the identity_plugin_VST3 target for this check"; exit 0; }

TMP="$(mktemp -d)"
SHM="/pinstall_$$"
ENG=""
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
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

IDPATH="$IDENTITY" python3 - "$TMP" <<'PY'
import json, os, sys, wave, struct, math
tmp = sys.argv[1]
Q = 960000; BAR = Q*4; sr = 48000
w = wave.open(os.path.join(tmp, "t.wav"), 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(12000*math.sin(2*math.pi*220.0*i/sr))) for i in range(sr//4)))
w.close()
# A plugin cache with Identity at slot 0, so `add-device --plugin 0` names something real. An
# earlier attempt at this repro inserted against an EMPTY cache: the device resolved to nothing,
# no host was launched, no lock was ever held, and the run proved nothing while looking clean.
json.dump({"schema_version": 1, "generated_at_ms": 0, "plugins": [{
    "path": os.environ["IDPATH"], "plugin_id_string": "VST3-Identity-c8a85e00-a5058c4c",
    "plugin_uid16": "78a8bd45ed06341c8c32d4642b098c3a", "name": "Identity", "vendor": "daw",
    "version": "0.1.0", "category": "Fx", "has_editor": False, "is_instrument": False,
    "num_inputs": 2, "num_outputs": 2}]},
    open(os.path.join(tmp, "cache.json"), "w"))

def route(k="none", t=0): return {"kind": k, "track_id": t, "input_id": 0}
slot = {"id":1,"name":"s","source_local_id":1,"slice_id":0,"start_frame":0,"end_frame":0,
        "loop_start_frame":0,"loop_end_frame":0,"loop_xfade_frames":0,"loop_mode":0,
        "sustain_loop":0,"key_low":0,"key_high":127,"root_key":60,"pitch_track_milli":0,
        "tune_cents":0,"vel_low":0,"vel_high":127,"layer_group":0,"select_mode":0,"gate":0,
        "reverse":0,"gain_millibels":0,"pan_thousandths":0,"voice_group":0,"nna":0,
        "polyphony":0,"choke_fade_us":3000,"mod_set_id":1,"output_stem":0,"quality":1}
sampler = {"device_id":1,"kind":"sampler","capability_mask":5,"patcher_node_id":0,
           "host_slot_index":0,"bypass":False,"sampler":{"next_slot_id":2,"next_source_id":2,
           "next_mod_set_id":2,"stem_count":0,"voice_cap":16,"default_view":0,
           "sources":[{"local_id":1,"path":"t.wav","content_key":0}],"slice_sets":[],
           "mod_sets":[{"id":1,"name":"d","filter_type":0,"cutoff_milli":1000,"resonance_milli":0,
                        "next_modulator_id":1,"modulators":[]}],"slots":[slot]}}
# A VST ALREADY ON THE TRACK, so the insert below takes the RECONCILE path — an existing host is
# told to rebuild its chain — which is what a person inserting into a live chain actually hits.
# Launching a first host is a different code path and does not hold this lock the same way.
vst = {"device_id":2,"kind":"vst_effect","capability_mask":2,"patcher_node_id":4294967295,
       "host_slot_index":0,"bypass":False,
       "vst_ref":{"vendor":"daw","name":"Identity","path":os.environ["IDPATH"],"uid16":""}}
notes = [{"nanotick": i*(Q//2), "duration": Q//4, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": i+1} for i in range(128)]
clip = {"id":1,"name":"c","length":BAR*16,"kind":"symbolic","notes":notes}
tr = {"track_id":0,"name":"T","harmony_quantize":False,"lines_per_beat":4,
      "mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},
      "routing":{"midi_in":route(),"midi_out":route(),"audio_in":route(),
                 "audio_out":route("master"),"pre_fader_send":True},
      "device_chain":[sampler, vst],"mod_links":[],
      "placements":[{"clip_id":1,"id":1,"at":0,"length":BAR*16,"notes":[],"chords":[],"mutes":[]}]}
json.dump({"schema_version":4,"meta":{"name":"ins"},"nanoticks_per_quarter":Q,
           "tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[clip],
           "tracks":[tr]}, open(os.path.join(tmp,"ins.uniproj.json"),"w"))
PY

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    DAW_PLUGIN_CACHE="$TMP/cache.json" DAW_ENGINE_DEBUG_STALL=1 DAW_ENGINE_NUM_BLOCKS="${DAW_ENGINE_NUM_BLOCKS:-2}" \
    ./daw_engine --project ins --run-seconds 45 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120 || fail "engine never came up:
$(tail -8 "$TMP/eng.log" | sed 's/^/          /')"

cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" 2>/dev/null; }
host_up() { grep -q "host ready for track 0" "$TMP/eng.log"; }
wait_until 90 host_up || fail "no host came up for track 0, so the insert would LAUNCH one rather
        than reconcile a live chain — a different code path from the one this check is about.
$(tail -8 "$TMP/eng.log" | sed 's/^/          /')"

cli do play >/dev/null 2>&1 || true
producing() { [ "$(grep -c 'producer stall' "$TMP/eng.log" 2>/dev/null)" -ge 0 ]; }
wait_until 20 producing >/dev/null 2>&1 || true
sleep 3   # a real duration: let the transport settle into steady state before disturbing it

BEFORE=$(grep -c "producer stall (inFlight)" "$TMP/eng.log" 2>/dev/null)
# The anchor goes in the ENGINE'S OWN LOG, so "after the insert" is a position in the same stream
# the stalls are counted from.
echo "insert-marker" >> "$TMP/eng.log"
cli do add-device --track 0 --kind vst_effect --plugin 0 >/dev/null 2>&1 || true
sleep 8

AFTER=$(awk '/insert-marker/{seen=1} seen && /producer stall \(inFlight\)/{n++} END{print n+0}' "$TMP/eng.log")
echo "  inFlight stalls before the insert: $BEFORE"
echo "  inFlight stalls after  the insert: $AFTER"
grep -oE "producer stall \(inFlight\)[^\"]*" "$TMP/eng.log" 2>/dev/null | tail -2 | sed 's/^/    /'

if [ "$AFTER" -gt 100 ]; then
  fail "the producer stayed gated after a plugin was inserted mid-playback ($AFTER inFlight stalls).

        This is the deadlock: the track whose controllerMutex the plugin load held was skipped
        for dispatch, rejoined the back-pressure minimum with a block id it can never improve on,
        and the closed gate is precisely what stops the dispatch that would let it catch up. The
        whole song goes silent, not just that track.

        The rule is daw::engine::completedMinimum in apps/engine_rt_helpers.h: a host that has
        completed everything DISPATCHED to it owes nothing and must not hold the gate shut.
        Check that TrackRuntime::lastDispatchedBlockId is still being stored in
        apps/engine_produce_block.cpp after a successful sendProcessBlock."
fi

echo "plugin_insert_stall_check: PASS — inserting a plugin mid-playback left the producer running"
