#!/usr/bin/env bash
# A PATCHER'S NOTES PLAY ITS OWN TRACK'S INSTRUMENT, AND NO OTHER TRACK'S.
#
# The live patcher pool is GLOBAL: reassemblePatcherFromDevices concatenates every track's
# per-device subgraph into one graph with re-id'd nodes (apps/daw_engine_main.cpp), and the same
# assembly runs on load (apps/engine_load_project.cpp). Ownership is therefore not structural —
# it is a filter applied at render time, and a filter that fails open is not a filter.
#
# It failed open. apps/engine_render_track.cpp decided whether to filter by asking whether THIS
# track carries a patcher device. A track carrying none set useNodeFilter=false, which does not
# select some other subset — it disables the ownership guard in runNode entirely and takes the
# branch that runs EVERY node in the pool, then merges their events into this track's scratchpad
# and so into this track's instrument.
#
# MEASURED, and this fixture is that measurement: a project where track 1 holds a sampler and NO
# patcher, NO clip, NO placement and NO note rendered track 0's generated notes out of track 1's
# sampler. Two tracks is not an exotic shape; it is the second thing anyone makes.
#
# WHY THE ONE-TRACK CONTROL RUNS FIRST. If the generator is dead, the two-track measurement is
# silent for a reason that has nothing to do with ownership, and silence would read as a pass
# forever. The control renders a single track carrying both the patcher and a sampler, and
# requires it to SOUND before the measurement below is allowed to mean anything. That control is
# also what keeps the fix honest in the other direction: closing the filter must not stop a
# patcher driving the instrument sitting next to it.
#
# WHY THE SPECTRUM AND NOT THE PANNING. "Track 1 played track 0's events" and "track 0 played
# correctly and was panned to the wrong side" produce identical left/right readings, and a
# pan-law inconsistency was alleged in the same review that raised this. So the two samplers load
# DIFFERENT tones — 300 Hz on track 0, 900 Hz on track 1 — and the assertion is on which PITCH
# came out. A tone names its source in a way a channel cannot.
#
# Offline render, so this is byte-deterministic and needs no audio device.
#   tools/patcher_track_scope_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
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

# TWO TONES so the spectrum can name which sampler sounded.
python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
for name, freq in (("s0.wav", 300.0), ("s1.wav", 900.0)):
    w = wave.open(os.path.join(sys.argv[1], name), 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(b''.join(
        struct.pack('<h', int(14000 * math.sin(2 * math.pi * freq * i / sr)
                              * max(0.0, 1 - i / 24000)))
        for i in range(sr)))
    w.close()
PY

# project <out> <mode>    mode: "control" = one track, patcher + its own sampler
#                               "measure" = two tracks, patcher on 0, bare sampler on 1
project() {
  python3 - "$1" "$Q" "$2" <<'PY'
import json, sys
out, Q, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]
DIRECT = 4294967294
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}

def sampler_device(dev_id, wav):
    slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
            "start_frame": 0, "end_frame": 0,
            "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0,
            # A zone across the keyboard: the pitch a patcher resolves is not known in advance.
            "key_low": 0, "key_high": 127, "root_key": 60,
            "pitch_track_milli": 1000, "tune_cents": 0,
            "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
            "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
            "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
            "mod_set_id": 1, "output_stem": 0, "quality": 1}
    return {"device_id": dev_id, "kind": "sampler", "capability_mask": 5,
            "patcher_node_id": 0, "host_slot_index": 0, "bypass": False,
            "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                        "stem_count": 0, "voice_cap": 16, "default_view": 0,
                        "sources": [{"local_id": 1, "path": wav, "content_key": 0}],
                        "slice_sets": [],
                        "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                      "cutoff_milli": 1000, "resonance_milli": 0,
                                      "next_modulator_id": 1, "modulators": []}],
                        "slots": [slot]}}

# euclidean -> random_degree -> event_out. The middle node is NOT decoration: euclidean emits
# GATES and the note path skips them, so without it the graph resolves nothing and renders
# silence that looks exactly like the defect under test.
patcher = {"device_id": 1, "kind": "patcher_instrument", "capability_mask": 5,
           "patcher_node_id": 1, "host_slot_index": DIRECT, "bypass": False,
           "vst_ref": {"vendor": "", "name": "", "path": "", "uid16": ""},
           "patcher": {"nodes": [
               {"id": 0, "type": "euclidean",
                "euclidean": {"steps": 16, "hits": 8, "offset": 0,
                              "duration_ticks": Q // 4, "degree": 1, "octave_offset": 0,
                              "velocity": 110, "base_octave": 4}},
               {"id": 2, "type": "random_degree",
                "random_degree": {"degree": 8, "velocity": 110, "duration_ticks": Q // 4}},
               {"id": 1, "type": "event_out"}],
             "edges": [
               {"src_node_id": 0, "src_port_id": 1, "dst_node_id": 2, "dst_port_id": 0,
                "kind": "event"},
               {"src_node_id": 2, "src_port_id": 1, "dst_node_id": 1, "dst_port_id": 0,
                "kind": "event"}]}}

def track(tid, name, chain):
    return {"track_id": tid, "name": name, "harmony_quantize": False, "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                        "audio_out": r("master"), "pre_fader_send": True},
            "device_chain": chain, "mod_links": [],
            # NO PLACEMENTS ANYWHERE IN THIS FILE. Every note in every render below came from
            # the patcher, so nothing here can be explained by a clip.
            "placements": []}

if mode == "control":
    tracks = [track(0, "PATCHER+SAMPLER", [patcher, sampler_device(2, "s0.wav")])]
else:
    tracks = [track(0, "PATCHER-ONLY", [patcher]),
              track(1, "SAMPLER-NO-PATCHER", [sampler_device(2, "s1.wav")])]

json.dump({"schema_version": 4, "meta": {"name": "scope"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}],
           # A harmony is REQUIRED: the resolution path turns a degree into a pitch through the
           # scale in force, and drops the event when there is none.
           "harmony_timeline": [{"nanotick": 0, "root": 0, "scale_id": 1}],
           "clips": [], "tracks": tracks}, open(out, "w"))
PY
}

# tone <name> <freq>  -> RMS energy at that frequency over the whole render (Goertzel)
tone() {
  python3 - "$TMP/$1.wav" "$2" <<'PY'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb'); freq = float(sys.argv[2])
sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
if n == 0:
    print("0.0"); raise SystemExit
mono = [sum(s[i*ch:(i+1)*ch]) / float(ch) for i in range(n)]
k = 2.0 * math.cos(2.0 * math.pi * freq / sr)
s1 = s2 = 0.0
for x in mono:
    s0 = x + k * s1 - s2
    s2, s1 = s1, s0
print("%.1f" % (math.sqrt(max(0.0, s1*s1 + s2*s2 - k*s1*s2)) / len(mono)))
PY
}

peak() {
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n = w.getnchannels(), w.getnframes()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
print(max((abs(v) for v in s), default=0))
PY
}

render() {  # render <projectName> <renderName>
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/pscope_${$}_$2" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --project "$1" --render "$2" --run-seconds 5 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
  grep -qE '"event":"project.patcher_(loaded|assembled)"' "$TMP/$2.log" || \
    fail "the patcher graph never assembled in '$2', so this render is measuring a project
        without one and every assertion about node ownership below would be vacuous"
}

# ---- CONTROL: one track, patcher + sampler. The generator must be alive and driving.
project "$TMP/ctl.uniproj.json" control
render ctl ctl
CP="$(peak ctl)"
C300="$(tone ctl 300)"
echo "  control  one track, patcher + sampler   -> peak $CP, 300Hz $C300"
[ "${CP:-0}" -gt 1000 ] || \
  fail "a patcher and a sampler on ONE track rendered silence (peak ${CP:-0}). The generator is
        not producing notes in this build, so the two-track measurement below would be silent
        for a reason that has nothing to do with which track owns which node"

# ---- MEASUREMENT: two tracks. Track 1 owns no patcher node and has nothing to play.
project "$TMP/two.uniproj.json" measure
render two two
TP="$(peak two)"
T900="$(tone two 900)"
echo "  measure  patcher on 0, bare sampler on 1 -> peak $TP, 900Hz $T900"

# 900 Hz is track 1's sampler and ONLY track 1's sampler. Track 0 carries no instrument in this
# project, so there is no legitimate source of audio in it at all.
if [ "${TP:-0}" -gt 1000 ]; then
  echo
  fail "track 1 sounded. It holds no patcher device, no clip, no placement and no note, and
        track 0 holds no instrument — so the only events in this project are the ones track 0's
        patcher generated, and they came out of track 1's sampler (peak $TP, energy at track 1's
        own 900 Hz = $T900).

        The pool is global and ownership is a render-time filter. A track with no patcher device
        must run NO pool nodes; if it instead runs all of them, every generator in the project
        plays every instrument in it."
fi

echo "patcher_track_scope_check: PASS — a patcher drives its own track's instrument (control),"
echo "                           and a track that owns no pool node stays silent"
