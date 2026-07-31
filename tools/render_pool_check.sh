#!/usr/bin/env bash
# THREADING THE PRODUCER MUST NOT CHANGE A SINGLE SAMPLE.
#
# The producer's per-track work runs on a pool (apps/render_pool.h) because measurement showed
# one thread runs out at roughly 40 sampler tracks — see tools/producer_load_check.sh. The whole
# argument that this is safe rests on one claim: the tracks that run in parallel are ISOLATED,
# meaning no track-to-track route reaches them, so no other track's work is observable to them
# and theirs is observable to no one. If that claim holds, the number of threads cannot change
# the output. If it does not hold, it changes it.
#
# So this check does not measure a speedup and does not sample the audio. It renders the same
# project on 1 thread and on many, and compares the files BYTE FOR BYTE. Anything short of
# identical is a failure, because "nearly the same audio" from a threading change means a race
# that happened not to be audible on this run.
#
# Floating-point accumulation is why byte-identity is the right bar rather than a tolerance:
# `+=` is not associative, so a genuine ordering change shows up as a last-bit difference long
# before it shows up as anything a human would hear. A tolerance would hide exactly the class of
# bug this exists to catch.
#
# Two shapes, because they take different paths through the partition:
#   ISOLATED   8 sampler tracks, no routing — every track lands in the parallel group
#   ROUTED     the same, but one track's audio is routed into another — both ends of the route
#              must fall back to the serial group, and the result must still match
#
# WHAT THE NEGATIVE CONTROL SHOWED, because it is not what I expected. Deliberately putting the
# routed tracks into the PARALLEL group leaves both shapes byte-identical: the ROUTED shape here
# cannot fail. That is a property of the engine, not a hole in the partition:
#   - a MIDI route stamps its events with nextBlockSampleStart, so it is delivered a block later
#     whatever order the tracks ran in — order-independent by construction;
#   - an audio route into a track that has a SAMPLER is overwritten, because the sampler feeds
#     the head of the chain with memcpy at segment 0 (daw_engine_main.cpp, "THE SAMPLER FEEDS
#     THE HEAD OF THE CHAIN"). The routed audio never survives to be heard;
#   - an audio route into a track with an EMPTY chain has no host to pass it through.
# So the only ordering-sensitive audio route is one into a track carrying a PLUGIN chain, which
# this check cannot fabricate without a plugin on the machine. The partition still puts both
# ends of every route in the serial group, which is the conservative choice and costs nothing.
#
# The control that DOES bite is a deliberate order-dependent perturbation in the parallel path —
# a shared counter scaling each track's sampler output by 1.000/1.001/1.002 in claim order. That
# makes 96528 bytes differ, which is how we know these comparisons can see an ordering change at
# all rather than passing because nothing is being compared.
#
#   tools/render_pool_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
# KEEP THE EVIDENCE ON FAILURE. This check compares whole renders byte for byte, and task #102 —
# an offline render whose first block or two depends on machine load — is OPEN and INTERMITTENT.
# When it fires, this check is one of the three things that notices. A trap that deletes the wavs
# on the way out turns the one occurrence anybody caught into "it failed once, and it passed when
# I ran it again", which is exactly how #102 stayed unexplained through two investigations.
KEEPDIR="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}"
keep_evidence() {
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    local dest="$KEEPDIR/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest (renders, projects and engine logs)"
  fi
  rm -rf "$TMP"
  exit $rc
}
trap keep_evidence EXIT
fail() { echo "  FAIL: $*"; exit 1; }

python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
w = wave.open(os.path.join(sys.argv[1], "s.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * 220.0 * i / sr)))
                       for i in range(sr * 2)))
w.close()
PY

# project <name> <trackCount> <routed>
project() {
  python3 - "$TMP/$1.uniproj.json" "$TMP" "$2" "$3" <<'PY'
import json, sys, os
out, dirname, ntracks, routed = (sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
Q = 960000
BAR = Q * 4
def routing(t):
    r = lambda k="none", tid=0: {"kind": k, "track_id": tid, "input_id": 0}
    # Track 0's audio into track 1 puts BOTH in the serial group; everything else is isolated.
    if routed and t == 0:
        return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                "audio_out": r("track", 1), "pre_fader_send": True}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
def slot(i, key):
    return {"id": i, "name": "s%d" % i, "source_local_id": 1, "slice_id": 0,
            "start_frame": 0, "end_frame": 0,
            "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0,
            "key_low": key, "key_high": key, "root_key": key,
            "pitch_track_milli": 1000, "tune_cents": 0,
            "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
            "gate": 0, "reverse": 0, "gain_millibels": -1200, "pan_thousandths": 0,
            "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
            "mod_set_id": 1, "output_stem": 0, "quality": 2}
clips, tracks = [], []
for t in range(ntracks):
    sampler = {
        "next_slot_id": 9, "next_source_id": 2, "next_mod_set_id": 2, "stem_count": 0,
        "voice_cap": 64, "default_view": 0,
        "sources": [{"local_id": 1, "path": os.path.join(dirname, "s.wav"),
                     "content_key": 0}],
        "slice_sets": [],
        "mod_sets": [{"id": 1, "name": "d", "filter_type": 1, "cutoff_milli": 8000,
                      "resonance_milli": 300, "next_modulator_id": 1, "modulators": []}],
        "slots": [slot(i + 1, 60 + i) for i in range(8)],
    }
    dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
           "host_slot_index": 0, "bypass": False, "sampler": sampler}
    # THE DESTINATION OF A ROUTE MUST NOT HAVE A SAMPLER. The sampler writes the input plane at
    # segment 0, so a destination that also has one overwrites the audio that was just routed
    # into it and the route carries nothing — which is exactly what the first version of this
    # fixture did. It passed with routed tracks deliberately put in the PARALLEL group, because
    # there was no routed signal for the ordering to affect. Track 1 is therefore an empty
    # track: with no plugins the host copies its input to its output, so what reaches the
    # master IS the routed audio, and whether it arrives this block or next is audible.
    chain = [dev]
    trackNotes = True
    if routed and t == 1:
        chain = []
        trackNotes = False
    step = Q // 4
    # Each track offset by one step so the tracks are NOT identical — identical tracks could
    # hide an ordering bug by producing the same samples whichever order they ran in.
    notes = [{"nanotick": n * step + (t % 4) * (step // 4), "duration": Q * 2,
              "pitch": 60 + ((n + t) % 8), "velocity": 90 + (t % 8) * 4,
              "column": n % 4, "note_id": n + 1}
             for n in range(int(BAR * 4 / step))]
    clips.append({"id": t + 1, "name": "p%d" % t, "length": BAR * 4, "kind": "symbolic",
                  "notes": notes if trackNotes else []})
    tracks.append({"track_id": t, "name": "T%d" % t, "harmony_quantize": False,
                   "lines_per_beat": 4,
                   "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
                   "routing": routing(t), "device_chain": chain, "mod_links": [],
                   "placements": [{"clip_id": t + 1, "id": t + 1, "at": 0,
                                   "length": BAR * 4, "notes": [], "chords": [],
                                   "mutes": []}]})
json.dump({"schema_version": 4, "meta": {"name": "pool"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": clips, "tracks": tracks}, open(out, "w"))
PY
}

# render <project> <outName> <threads>   — threads "auto" leaves the variable unset; any number
# forces exactly that many AND forces the pool on.
#
# The pooled runs pass an explicit 8 rather than "auto" deliberately. The pool now engages on the
# WORK — at eight sampler tracks one thread has room to spare and the adaptive rule correctly
# leaves it serial — so "auto" here would compare a serial render against a serial render and
# prove nothing at all. An explicit count means "I know what I want" and turns adaptation off,
# which is exactly what a check needs.
render() {
  local threadEnv=""
  [ "$3" = "auto" ] || threadEnv="DAW_ENGINE_RENDER_THREADS=$3"
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/pool_$$_$2" \
      ${threadEnv} \
      ./daw_engine --project "$1" --render "$2" --run-seconds 8 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

compare() {  # compare <label> <wavA> <wavB> <logB>
  local threads
  threads="$(grep -o 'Render pool: [0-9]* thread' "$4" | tail -1 | grep -o '[0-9]*')"
  # COMPARED WHOLE, with cmp. This briefly skipped a one-second lead-in while task #102 was
  # open — an offline render's first blocks depended on machine load, so `cmp` failed under a
  # parallel ctest and this check's message ("threading CHANGED THE AUDIO") would have been a
  # confident lie about the render pool. Fixed at the source; the whole file is compared again.
  if cmp -s "$2" "$3"; then
    echo "  $1: identical on 1 thread and ${threads:-?} ($(wc -c <"$2" | tr -d ' ') bytes)"
  else
    local diffbytes
    diffbytes="$(cmp -l "$2" "$3" 2>/dev/null | wc -l | tr -d ' ')"
    fail "$1: threading CHANGED THE AUDIO — $diffbytes byte(s) differ between a 1-thread
        render and a ${threads:-?}-thread one. The parallel group is supposed to contain only
        tracks that no route can reach, so the thread count cannot be observable. It was"
  fi
}

# ---- ISOLATED: no routing, so every track goes in the parallel group.
project iso 8 0
render iso iso1 1
render iso isoN 8
POOLN="$(grep -o 'Render pool: [0-9]* thread' "$TMP/isoN.log" | tail -1 | grep -o '[0-9]*')"
[ "${POOLN:-1}" -gt 1 ] || fail "the pool ran with ${POOLN:-1} thread(s), so the parallel path
        was never exercised and this check proves nothing. It needs a machine with >3 cores, or
        DAW_ENGINE_RENDER_THREADS set"
compare "isolated" "$TMP/iso1.wav" "$TMP/isoN.wav" "$TMP/isoN.log"

# ---- ROUTED: track 0's audio feeds track 1, so both must fall back to the serial group.
project rt 8 1
render rt rt1 1
render rt rtN 8
compare "routed  " "$TMP/rt1.wav" "$TMP/rtN.wav" "$TMP/rtN.log"

echo "render_pool_check: PASS — the thread count is not observable in the output"
