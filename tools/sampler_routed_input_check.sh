#!/usr/bin/env bash
# WHAT HAPPENS TO AUDIO ROUTED INTO A TRACK WHOSE INSTRUMENT IS A SAMPLER — pinned, not decided.
#
# The sampler feeds the HEAD of the chain, and it REPLACES the track's input rather than mixing
# into it. So a track that is both an instrument and a bus destination silently loses everything
# routed in. The engine already says so — `sampler.discarded_routed_input`, once per track — rather
# than letting the audio disappear with nothing to look at.
#
# THIS CHECK DOES NOT DECIDE THE QUESTION. Whether a sampler track should MIX instead is a real
# decision about what a track IS: Live and Renoise both mix, and docs/SAMPLER_DESIGN.md records it
# as task #92, "deliberately not decided by the work". Nothing here argues either way.
#
# WHAT IT DOES IS MAKE THE CURRENT ANSWER VISIBLE AND THE FUTURE ONE LOUD. Until this file the
# behaviour was a comment and a log line nobody asserted on — so a change either way would have
# landed silently, in either direction: someone making sampler tracks mix would have seen a green
# suite, and so would someone accidentally breaking the routing that feeds them. Now the day the
# decision is made, THIS check fails, and its message says it is the #92 decision rather than a
# regression.
#
# WHAT IT ASSERTS IS THE DIAGNOSTIC, NOT THE SPECTRUM, and the reason is worth writing down because
# I built the spectral version first and it could not tell the two cases apart.
#
# `audio_out: track X` IS ADDITIVE. The destination gets a copy in its inbound buffer, and the
# source track STILL reaches the master — nothing in the mix consults audioOut.kind at all. Measured
# three ways over the same fixture, energy at track 1's own 900 Hz:
#
#     track 1 -> master                        300Hz 760   900Hz 761
#     track 1 -> track 0 (sampler on track 0)  300Hz 760   900Hz 761
#     track 1 -> track 0 (track 0 EMPTY)       300Hz   1   900Hz 760
#
# The routed copy contributes nothing to the master in EITHER case, so a spectral test cannot see
# the discard: the numbers are identical whether the sampler eats the input or not. A check built on
# that difference would have been measuring nothing and passing for it — which is the exact defect
# this repo keeps finding, and the reason the numbers are in this comment rather than in my head.
#
# The engine's own report is the observable, and it is also the thing that matters to a musician:
# the value of the current behaviour is not that the audio is dropped, it is that dropping it says
# so out loud.
#
# Needs the engine built. Renders offline, so no audio device and no wall clock.
#   tools/sampler_routed_input_check.sh
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

# TWO TONES so the spectrum can name which track sounded. Track 0 is 300 Hz, track 1 is 900 Hz.
python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
for name, freq in (("s0.wav", 300.0), ("s1.wav", 900.0)):
    w = wave.open(os.path.join(sys.argv[1], name), 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    # A long, slow decay: the note must still be sounding when the render samples it.
    w.writeframes(b''.join(
        struct.pack('<h', int(14000 * math.sin(2 * math.pi * freq * i / sr)
                              * max(0.0, 1 - i / float(sr))))
        for i in range(sr)))
    w.close()
PY

# project <out> <mode>   mode: "master" = track 1 -> master (the control)
#                              "into"   = track 1 -> track 0, whose instrument is a sampler
project() {
  python3 - "$1" "$Q" "$2" <<'PY'
import json, sys
out, Q, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]
BAR = Q * 4

def route(kind="none", track_id=0):
    return {"kind": kind, "track_id": track_id, "input_id": 0}

def sampler_device(dev_id, wav):
    slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
            "start_frame": 0, "end_frame": 0,
            "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0,
            "key_low": 0, "key_high": 127, "root_key": 60,
            "pitch_track_milli": 0,  # FIXED PITCH: the tone must stay at its own frequency, or
                                     # the spectrum cannot name which track it came from.
            "tune_cents": 0,
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

def clip(cid, note_id):
    return {"id": cid, "name": "c%d" % cid, "length": BAR, "kind": "symbolic",
            "notes": [{"nanotick": 0, "duration": BAR, "pitch": 60, "velocity": 110,
                       "column": 0, "note_id": note_id}]}

def track(tid, wav, out_route, clip_id):
    return {"track_id": tid, "name": "T%d" % tid, "harmony_quantize": False,
            "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                        "audio_out": out_route, "pre_fader_send": True},
            "device_chain": [sampler_device(1, wav)], "mod_links": [],
            "placements": [{"clip_id": clip_id, "id": clip_id, "at": 0, "length": BAR,
                            "notes": [], "chords": [], "mutes": []}]}

# Track 1's destination is the whole difference between the two renders.
dest = route("master") if mode == "master" else route("track", 0)
t0 = track(0, "s0.wav", route("master"), 1)
if mode == "passthru":
    t0["device_chain"] = []      # no instrument at all: whatever arrives is all it can emit
    t0["placements"] = []
tracks = [t0, track(1, "s1.wav", dest, 2)]

json.dump({"schema_version": 4, "meta": {"name": "rin"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip(1, 1), clip(2, 2)], "tracks": tracks}, open(out, "w"))
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
    print("0"); raise SystemExit
mono = [sum(s[i*ch:(i+1)*ch]) / float(ch) for i in range(n)]
k = 2.0 * math.cos(2.0 * math.pi * freq / sr)
s1 = s2 = 0.0
for x in mono:
    s0 = x + k * s1 - s2
    s2, s1 = s1, s0
print("%d" % int(math.sqrt(max(0.0, s1*s1 + s2*s2 - k*s1*s2)) / len(mono)))
PY
}

render() {  # render <projectName> <renderName>
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/srin_${$}_$2" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --project "$1" --render "$2" --run-seconds 4 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

# ---- THE CONTROL FIRST: nothing routed anywhere, so nothing may be reported. A check that only
# asserts a line APPEARS passes just as well against an engine that prints it unconditionally.
project "$TMP/tomaster.uniproj.json" master
render tomaster tomaster
M300="$(tone tomaster 300)"
M900="$(tone tomaster 900)"
echo "  control  both tracks -> master        : 300Hz $M300, 900Hz $M900"
[ "${M300:-0}" -gt 20 ] && [ "${M900:-0}" -gt 20 ] || \
  fail "one of the two samplers is not sounding in the control render (300Hz $M300, 900Hz $M900).
        Both must play, or the routed render below proves nothing about routing — it would just be
        a project that does not make sound"
grep -q '"event":"sampler.discarded_routed_input"' "$TMP/tomaster.log" && \
  fail "the engine reported discarding routed input in a project where NOTHING is routed into a
        sampler track. The report is unconditional, so its presence in the routed render below
        would say nothing at all"

# ---- MEASUREMENT: track 1 goes INTO track 0, whose instrument is a sampler.
project "$TMP/intotrack.uniproj.json" into
render intotrack intotrack
I300="$(tone intotrack 300)"
I900="$(tone intotrack 900)"
echo "  measure  track 1 -> track 0 (sampler) : 300Hz $I300, 900Hz $I900"

# Track 0's own sampler must still sound. Replacing the INPUT must leave the sampler's own output
# alone, and if track 0 fell silent the report below could be about a track that stopped working.
[ "${I300:-0}" -gt 20 ] || \
  fail "track 0's own 300 Hz vanished when track 1 was routed into it (energy $I300). Replacing
        the input should leave the sampler's own output untouched, whichever way #92 goes"

# THE ASSERTION. This is what changes the day #92 is decided.
grep -q '"event":"sampler.discarded_routed_input"' "$TMP/intotrack.log" || \
  fail "audio was routed into a sampler track and the engine said NOTHING about it.

        Either the sampler now MIXES the routed input — which is the #92 decision, and then THIS
        CHECK is what needs updating rather than the engine: say which way it went, assert the new
        behaviour, and update docs/SAMPLER_DESIGN.md where it is recorded as open —

        or the discard is still happening and the only evidence a musician had that a send into a
        sampler track goes nowhere has been lost. The audio then disappears with nothing to look
        at, which is the shape this project keeps paying for. Log: $TMP/intotrack.log"

# It is latched per track, not per block: a standing routing choice reported once a block is a
# diagnostic that drowns the log it belongs in.
N="$(grep -c '"event":"sampler.discarded_routed_input"' "$TMP/intotrack.log")"
[ "$N" = "1" ] || \
  fail "the discard was reported $N times, expected exactly 1. It is latched per track on purpose"

grep '"event":"sampler.discarded_routed_input"' "$TMP/intotrack.log" | grep -q '"track":0' || \
  fail "the discard was reported for the wrong track — track 0 is the sampler being fed:
        $(grep '"event":"sampler.discarded_routed_input"' "$TMP/intotrack.log" | tail -1)"

echo "sampler_routed_input_check: PASS — a sampler REPLACES audio routed into its track and says so" \
     "once, naming the track (#92 is still open, and this check is what will notice when it is not)"
