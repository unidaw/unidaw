#!/usr/bin/env bash
# THE WHOLE FILE IS REACHABLE, AND A NEW SAMPLER EXISTS BEFORE IT HAS ANYTHING IN IT.
#
# Two defects the web-UI agent found from the outside, both invisible from in here because
# everything the engine could see about them was self-consistent.
#
# ONE: `--count 8` MADE SEVEN PLAYABLE SLICES. divideEqually returned the INTERIOR boundaries,
# and sliceExtentAt begins every slice AT a marker — so with markers at f1..f7 the audio in
# [0, f1) had no index, no id, and no way to be played. A comment called frame 0 "the first
# slice's implicit start"; it was not implicit, it was unreachable, and for an equal division
# the unreachable part is the DOWNBEAT.
#
# It was reported as an off-by-one in the count. It was not — the count was right and the head of
# the file was being thrown away, which is what made it look like one slice short. Worth the
# distinction: fixing the count would have produced eight markers and still lost the first
# region.
#
# TWO: A SAMPLER ADDED THROUGH AddDevice HAD NO SNAPSHOT. refreshSamplerForTrack claims in its
# own comment to be "called from EVERY site that changes a chain" and the site that ADDS devices
# was not one of them. So the instrument was not installed on the audio thread, and its kit
# read-back answered found:false — the same answer as "there is no sampler on that device". That
# is exactly the interval, created but not yet loaded, when a UI most wants to say "here it is,
# put something in it".
#
# FOUR PROPERTIES:
#   EXISTS      a freshly added sampler reports found, before anything is loaded into it
#   RESOLVES    a bare name reaches the project's sibling audio/ directory
#   COVERS      chopping into N yields N slices, the first beginning at frame 0
#   PLAYS       the first slice makes sound. Ids that exist and play nothing would satisfy the
#               property above and still lose the downbeat
#
#   tools/sampler_chop_reach_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000
PARTS=8

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
# THE REPO'S OWN LAYOUT: projects in one directory, samples in a sibling `audio/`. Every project
# in presets/ references its audio as "../audio/<name>", and that nine-byte prefix eats most of
# the load command's twenty-four name bytes — so the sample here is reachable only if a bare name
# also looks in the project's audio directory.
mkdir -p "$TMP/projects" "$TMP/audio"
ENG=""
# stop_engine, not a bare kill: it SIGTERMs, waits 10s, ESCALATES to SIGKILL and SAYS SO.
# A hung engine that ignores SIGTERM is left running by a bare kill, and ctest then waits
# about 1000s for it after timing the check out — measured across 18 runs, perfectly
# correlated with the timeout count. The escalation notice is also the diagnostic: "engine
# N ignored SIGTERM for 10s" names the hang instead of leaving it to be inferred.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }
. "$ROOT/tools/lib/engine_wait.sh"

# A BREAK WHOSE FIRST EIGHTH IS THE LOUD ONE and the rest are quiet. That asymmetry is the whole
# fixture: if the first slice is unreachable, playing slice 1 gives you the quiet material and
# the peak collapses. A file of eight identical hits would sound the same either way and prove
# nothing at all.
python3 - "$TMP/audio/s.wav" "$PARTS" <<'PY'
import sys, wave, struct, math
sr = 48000
parts = int(sys.argv[2])
n = sr * 2
seg = n // parts
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
fr = []
for i in range(n):
    which = min(i // seg, parts - 1)
    inseg = i % seg
    env = max(0.0, 1.0 - inseg / (seg * 0.5))
    amp = 15000 if which == 0 else 1200
    fr.append(struct.pack('<h', int(amp * math.sin(2 * math.pi * 300.0 * i / sr) * env)))
w.writeframes(b''.join(fr)); w.close()
PY

python3 - "$TMP/projects/c.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
# NO DEVICES. The sampler is ADDED by a command, which is the path that had no snapshot.
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "c"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

export DAW_UI_SHM_NAME="/chopreach_$$" DAW_PROJECT_DIR="$TMP/projects"
( cd "$BUILD" && exec ./daw_engine --project c --run-seconds 30 >"$TMP/projects/eng.log" 2>&1 ) &
ENG=$!
# WAITS FOR THE PROJECT, NOT FOR THE THREADS. "starting threads" is printed before the startup
# project has been loaded, so a command sent on that signal can arrive at an engine whose tracks
# do not exist yet — and it is REFUSED, with a reason, into the engine's log where nothing here
# was looking. With two tracks the load takes longer and the race became reliable: every
# sampler-load came back "no_sampler_device" and the check reported that the read-back returned
# nothing, which was true and about the wrong thing.
wait_for_boot "$TMP/projects/eng.log" "$ENG" 160
grep -q '"event":"project.load"' "$TMP/projects/eng.log" 2>/dev/null || fail "the engine never loaded its project"

kit() { "$CLI" get sampler-kit --track 0 2>/dev/null; }
waitfor() {  # waitfor <grep-pattern>
  for _ in $(seq 1 40); do
    kit | grep -qE "$1" && return 0
    sleep 0.25
  done
  return 1
}

# ---- EXISTS. Before anything is loaded.
"$CLI" do add-device --track 0 --kind sampler >/dev/null 2>&1
waitfor '"found": true' || \
  fail "a sampler added through add-device reports found:false — the same answer as 'there is no
        sampler there'. AddDevice changes the chain and did not refresh the sampler, so the
        instrument was never installed on the audio thread at all"
echo "  a freshly added sampler reports found, with nothing in it yet"


# ---- RESOLVES. The name is bare and the file is in the sibling audio/ directory, which is
# where every project in this repo keeps its samples. "../audio/" is nine of the twenty-four name
# bytes the command has, leaving fifteen for a filename — so without the audio-directory fallback
# the repo's own sample cannot be named by this command at all.
"$CLI" do sampler-load --track 0 --file s.wav --root 60 >/dev/null 2>&1
waitfor '"slot": 1' || fail "the sample never loaded — see $TMP/projects/eng.log"
grep -q '"event":"sampler.source_missing"' "$TMP/projects/eng.log" && \
  fail "a bare name did not resolve to the project's sibling audio/ directory, so the slot exists
        and is silent. That directory is where every project here keeps its audio, and naming it
        inline costs nine of the command's twenty-four name bytes"
echo "  a bare name resolved to the sibling audio/ directory"

# ---- COVERS.
"$CLI" do sampler-slice --track 0 --source 1 --mode equal --count "$PARTS" >/dev/null 2>&1
# --slots is not passed: it now defaults ON, and this check is one of the things that says so.
sleep 1.5
MADE="$(grep -oE '"event":"sampler.sliced"[^}]*' "$TMP/projects/eng.log" | tail -1 |
        grep -oE '"made":[0-9]+' | grep -oE '[0-9]+')"
SLOTS="$(grep -oE '"event":"sampler.sliced"[^}]*' "$TMP/projects/eng.log" | tail -1 |
         grep -oE '"slots":[0-9]+' | grep -oE '[0-9]+')"
[ "${MADE:-0}" = "$PARTS" ] || \
  fail "chopping into $PARTS made ${MADE:-0} slices. divideEqually returned the INTERIOR
        boundaries, so the region from frame 0 to the first marker had no id and could not be
        played — the count was right and the head of the file was being dropped"
[ "${SLOTS:-0}" = "$PARTS" ] || \
  fail "chopping into $PARTS made ${SLOTS:-0} slots against $MADE slices. One slot per slice is
        what makes a chop playable in one command"
echo "  chopping into $PARTS made $MADE slices and $SLOTS slots (slots default on)"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

# ---- PLAYS. The first slice specifically, because that is the one that was unreachable.
#
# Rendered from a project the CHECK writes with the chop already in it, rather than by driving
# the engine mid-render: an offline render does not wait for commands, and editing while voices
# sound is its own subject (tools/sampler_edit_while_playing_check.sh).
python3 - "$TMP/projects/p.uniproj.json" "$Q" "$PARTS" <<'PY'
import json, sys
out, Q, parts = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
frames = 48000 * 2
markers = [{"id": i + 1, "frame": frames * i // parts} for i in range(parts)]
# ONE SLOT, PLAYING SLICE 1 — the first marker, at frame 0. If that slice's extent is wrong or
# its marker never existed, this renders the quiet material instead of the loud first hit.
slot = {"id": 1, "name": "s1", "source_local_id": 1, "slice_id": 1,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 0, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "s.wav", "content_key": 0}],
                   "slice_sets": [{"source_local_id": 1, "next_marker_id": parts + 1,
                                   "markers": markers}],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                                 "resonance_milli": 0, "next_modulator_id": 1,
                                 "modulators": []}],
                   "slots": [slot]}}
notes = [{"nanotick": i * Q, "duration": Q // 2, "pitch": 60, "velocity": 120,
          "column": 0, "note_id": i + 1} for i in range(4)]
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "p"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY

( cd "$BUILD" && env DAW_UI_SHM_NAME="/chopplay_$$" DAW_PROJECT_DIR="$TMP/projects" \
    ./daw_engine --project p --render first --run-seconds 5 --block-size 256 \
    >"$TMP/projects/play.log" 2>&1 ) || fail "the render exited non-zero — see $TMP/projects/play.log"
[ -s "$TMP/projects/first.wav" ] || fail "the render wrote no output"

PEAK="$(python3 - "$TMP/projects/first.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n = w.getnchannels(), w.getnframes()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
print(max((abs(v) for v in s), default=0))
PY
)"
echo "  slice 1 renders at peak $PEAK"
# The first eighth is the loud one at 15000; every other eighth is 1200. A slice that resolved to
# the wrong region, or to nothing, lands far below this.
[ "${PEAK:-0}" -gt 6000 ] || \
  fail "playing slice 1 gave peak ${PEAK:-0}. The fixture makes the FIRST eighth loud and the
        rest quiet, so a low peak means slice 1 is not the head of the file — either its marker
        at frame 0 was refused, or its extent resolved somewhere else"

echo "sampler_chop_reach_check: PASS — the whole file is reachable and a new sampler exists"
