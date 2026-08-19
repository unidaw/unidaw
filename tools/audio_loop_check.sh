#!/usr/bin/env bash
# Checks that AUDIO loops with the arrangement, like notes always have.
#
# The note transport wraps at the arrangement end; the audio callback's sample position
# did not, so an audio clip played ONCE and that track was silent for the rest of the
# session. Measured before the fix: 24 seconds of a 4-second loop produced exactly one
# click while the tracker played every pass. Place a drum loop in an arrangement, hit
# play, and it hits once — which reads as "the sample is broken", not as a transport bug.
#
# Both properties come from ONE mechanism: the producer stamps each block with where it
# sits on the timeline, and the callback positions audio by the block it is actually
# playing. Looping falls out because that stamp is a looping transport position; alignment
# falls out because it is the position of the very block whose MIDI is being mixed.
#
# The first version of this fix wrapped a counter of its own at the loop point, and the
# first version of this CHECK compared audio gaps to each other — which passed a
# deliberately broken wrap, because a wrong-but-consistent loop length is consistent.
# So what is measured here is the SEPARATION BETWEEN AUDIO AND NOTES, pass after pass:
# an audio click panned hard LEFT and a note-driven pulse at the same tick hard RIGHT, one
# per channel, compared. Audio drifting from the notes is worse than audio not looping at
# all, because it is not obviously broken.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/audio_loop_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/identity_plugin.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((4 * Q))
LOOP_SECONDS=4.0     # two bars at 120 bpm

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
SHM="/alchk_$$"
# KEEP THE EVIDENCE WHEN IT FAILS. This used to be `trap 'rm -rf "$TMP"' EXIT`, while the failure
# messages above tell you to read a log inside $TMP — so the one run whose log you need is the one
# run that deletes it. That is not hypothetical: audio_loop failed once under a full-suite run,
# passed 9 times in isolation, and the reason is gone. Same convention as elektron_ops_check.
KEEPDIR="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}"
keep_evidence() {
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    local dest="$KEEPDIR/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  rm -rf "$TMP"
  exit $rc
}
trap keep_evidence EXIT

python3 - "$TMP/click.wav" <<'PY'
import sys, wave, struct
sr = 44100
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', 28000 if i < sr // 50 else 0)
                       for i in range(sr // 4)))
w.close()
PY

# WHERE THE BUNDLE IS, asked of tools/lib/identity_plugin.sh rather than typed. The flat
# `identity_plugin_artefacts/VST3/` path this used to hardcode is a layout JUCE stopped
# emitting long ago (tools/webstack.sh records the same finding); it survives only as a
# leftover in build directories that were configured before the change, so on a FRESH
# checkout this check bailed with "build identity_plugin first" and read as a regression.
IDENTITY="$(resolve_identity_vst3 "$BUILD")" || IDENTITY="$BUILD/identity_plugin_artefacts/VST3/Identity.vst3"
[ -d "$IDENTITY" ] || { echo "build identity_plugin_VST3 first"; exit 2; }

# An audio click hard LEFT and a note-driven pulse hard RIGHT, both at tick 0 of a
# two-bar arrangement. The arrangement end (and so the loop) is the furthest placement
# end. The instrument is the repo's Identity fixture, which emits a short pulse per
# note-on — a sharp onset to compare against.
cat > "$TMP/al.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "al" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [
    { "id": 1, "name": "click", "length": $Q, "kind": "audio",
      "audio": { "source_path": "$TMP/click.wav", "source_start_frame": 0,
                 "gain_db": 0.0, "fade_in": 0, "fade_out": 0 } },
    { "id": 2, "name": "span", "length": $((2 * BAR)), "kind": "symbolic", "notes": [
      { "nanotick": 0, "duration": 120000, "pitch": 72, "velocity": 120,
        "column": 0, "note_id": 1 } ] } ],
  "tracks": [
    { "track_id": 0, "name": "Aud",
      "mixer": { "gain_db": 0.0, "pan": -1.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "placements": [ { "clip_id": 1, "id": 1, "at": 0, "length": $Q,
                        "notes": [], "chords": [], "mutes": [] } ] },
    { "track_id": 1, "name": "Note",
      "mixer": { "gain_db": 0.0, "pan": 1.0, "mute": false, "solo": false },
      "device_chain": [
        { "device_id": 1, "kind": "vst_instrument", "capability_mask": 5,
          "patcher_node_id": 4294967295, "host_slot_index": 4294967294, "bypass": false,
          "vst_ref": { "vendor": "daw", "name": "Identity", "path": "$IDENTITY", "uid16": "" } } ],
      "mod_links": [],
      "placements": [ { "clip_id": 2, "id": 2, "at": 0, "length": $((2 * BAR)),
                        "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

# RENDERED OFFLINE. What this check is about — where a clip retriggers when the transport wraps
# — is timeline arithmetic, and it is measured from onset POSITIONS, which a render gives exactly
# instead of approximately. The capture ring is gone with it: the old comment above had to warn
# that a ring shorter than the run silently drops the beginning, which is where the first onset
# is. A render has no ring, no start transient, and no dropouts to mistake for a missed retrigger.
START=$SECONDS
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project al --render out --sample-rate 44100 --run-seconds 24 \
    >"$TMP/engine.log" 2>&1 ) \
  || { echo "  FAIL: the render exited non-zero — see $TMP/engine.log"; exit 1; }
[ -s "$TMP/out.wav" ] || { echo "  FAIL: the render wrote no output"; exit 1; }
echo "  rendered 24 s of audio in ~$((SECONDS - START)) s of wall time, no audio device"

cat > "$TMP/analyse.py" <<'PYA'
import sys, wave, numpy as np

path, loop_seconds = sys.argv[1], float(sys.argv[2])
w = wave.open(path, 'rb'); sr = w.getframerate(); ch = w.getnchannels()
raw = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
if ch < 2:
    print("  FAIL (setup): the capture is mono; the two sources cannot be separated")
    raise SystemExit(1)
frames = raw.reshape(-1, ch)
left, right = frames[:, 0], frames[:, 1]

def onsets(sig, label):
    peak = float(np.max(np.abs(sig)))
    if peak < 0.02:
        print("  FAIL (setup): the %s channel is silent (peak %.4f)" % (label, peak))
        return None
    loud = np.abs(sig) > peak * 0.4
    out = []
    i = 0
    refractory = int(0.3 * sr)
    while i < len(loud):
        if loud[i]:
            out.append(i / sr)
            i += refractory
        else:
            i += 1
    return out

audio = onsets(left, "audio (left)")
notes = onsets(right, "note (right)")
if audio is None or notes is None:
    raise SystemExit(1)

print("  audio onsets: %d   note onsets: %d   over %.1f s"
      % (len(audio), len(notes), len(left) / sr))

ok = True
if len(audio) < 4:
    print("  FAIL: the audio clip sounded %d time(s) in a %.1f s capture of a %.1f s"
          % (len(audio), len(left) / sr, loop_seconds))
    print("        loop — audio is not looping with the arrangement (the sample position")
    print("        runs on past the loop end while the notes wrap)")
    ok = False
if len(notes) < 4:
    print("  FAIL (setup): the NOTE side sounded %d time(s); it is the reference and has"
          % len(notes))
    print("        always looped, so this is a broken fixture rather than a finding")
    ok = False

# A MISSING HEAD ONSET IS NOT A MEASURED SKEW, and this reported it as one.
#
# The pairing below is POSITIONAL. On the intermittent failure, the note side had nothing at
# t=0, so audio[0]=0.000 paired with notes[0]=3.998 and every separation came out at exactly one
# loop length. The check announced "the first pass is -4000.0 ms out", which reads as a transport
# bug and sends the reader looking for one. Nothing was 4 seconds out; the streams had different
# lengths and were compared as if they did not.
#
# COUNTING IS NOT THE TEST. A healthy render of N loops has N audio onsets and N+1 note onsets
# (the note at the far edge sounds, the audio pass containing it does not finish), and the
# failing render had SIX of each — equal counts, offset sets. What separates the two is whether
# the FIRST audio onset has a note beside it, which is the alignment this check is about.
#
# AND THE DIAGNOSIS STOPS THERE, deliberately. A dropped head note and a note stream one loop
# late are INDISTINGUISHABLE in a fixed-length render: the shift loses its last onset to the
# render edge exactly as the drop loses its first, so both leave the same six timestamps. The
# check reports the observation and names both readings rather than picking the one that sounds
# more like a finding.
if ok and audio and notes:
    first_gap = min(abs(audio[0] - t) for t in notes)
    if first_gap > loop_seconds * 0.25:
        ok = False
        print("  FAIL: nothing sounded on the note side at the head of the render. The audio at"
              )
        print("        %.3f s has no note within %.3f s; the nearest is %.3f s away."
              % (audio[0], loop_seconds * 0.25, first_gap))
        print("        TWO READINGS FIT AND NOTHING CAN SEPARATE THEM: the head note may have been")
        print("        dropped, or the whole note stream may be one loop late. This is not a limit")
        print("        of the measurement, it is an identity — the arrangement LOOPS, so the note")
        print("        stream is periodic, and shifting a periodic stream by exactly one period")
        print("        maps it onto itself. The two hypotheses predict the same onsets at every")
        print("        render length, so no longer render and no extra note can tell them apart.")
        print("        (An earlier version of this message suggested rendering longer and counting.")
        print("        That was wrong: a shift loses its last onset to truncation exactly as fast")
        print("        as it gains one at the head, so the count matches the drop at any length.)")
        print("        IT ALSO DOES NOT MATTER, which is the useful half: under either reading a")
        print("        note that should have sounded at the head did not, and the thing to find is")
        print("        why the first note fails to reach the instrument.")
        print("        What is certain either way: the separations printed below are an artefact of")
        print("        pairing two streams by index, not a measurement of skew.")

# Pair each audio onset with the nearest note onset and watch the SEPARATION. Both
# sources sit at tick 0 of the loop, so in a correctly aligned engine the separation
# is a small constant; if audio is positioned by anything other than the block it is
# actually in, it is a large constant that MOVES with the pipeline depth.
#
# Printed whenever there is anything to print, INCLUDING after a failure above — the head-onset
# message tells the reader these numbers are an artefact, and a message that refers to output
# the failure path suppressed would be worse than no message.
seps = []
if audio and notes:
    pairs = min(len(audio), len(notes))
    seps = [audio[k] - notes[k] for k in range(pairs)]
    print("  audio-note separation per pass: %s"
          % ", ".join("%+.4f" % x for x in seps))

if ok and seps:
    # PASS 1 IS NOW ASSERTED, and that is a consequence of rendering offline rather than
    # capturing. It used to be reported and excused: in realtime, audio can begin before the
    # MIDI side has primed its pipeline (no host is "active" yet, so the priming gate does not
    # engage), which put the first pass out by the priming depth. The render pump never primes
    # with silence and never skips a block, so there is no transient to excuse — measured
    # +0.0000 exactly. Asserting it is what keeps that property from quietly regressing, since
    # the excuse would otherwise absorb a real first-pass bug forever.
    print("  pass 1: %+.4f s" % seps[0])
    if abs(seps[0]) > 0.020:
        print("  FAIL: the first pass is %.1f ms out." % (seps[0] * 1000.0))
        print("        A render has no startup transient to blame: it never primes with")
        print("        silence and never skips a block, so pass 1 is aligned or it is a bug.")
        ok = False

    steady = seps[1:]
    if len(steady) < 3:
        print("  FAIL (setup): only %d steady-state passes; need at least 3" % len(steady))
        ok = False
    else:
        worst = max(abs(x) for x in steady)
        spread = max(steady) - min(steady)
        print("  steady state: worst |separation| %.4f s, spread %.4f s" % (worst, spread))
        # One block is ~11.6 ms at 512/44100. Audio must sit within a block of its own
        # MIDI, and — the property that actually broke — that must not depend on how
        # deep the pipeline is. Positioning audio by an independent counter measured
        # 19 ms out at 3 blocks, 75 ms at 8 and 169 ms at 16, so this threshold fails
        # every one of those while passing a correct engine at any depth.
        if worst > 0.020:
            print("  FAIL: audio sits %.1f ms from its own MIDI in steady state." % (worst * 1000.0))
            print("        If that number tracks DAW_ENGINE_NUM_BLOCKS, audio is being")
            print("        positioned by a counter of its own instead of by the block it")
            print("        is in — and it moves whenever that setting is tuned.")
            ok = False
        if spread > 0.020:
            print("  FAIL: the separation is walking by %.1f ms across passes" % (spread * 1000.0))
            ok = False

    gaps = [audio[k + 1] - audio[k] for k in range(len(audio) - 1)]
    print("  audio gaps: %s" % ", ".join("%.3f" % g for g in gaps))
    # Every gap, including the first: offline there is no startup transient for pass 1 to
    # begin early inside, so the first loop period is as assertable as the rest.
    for g in gaps:
        if abs(g - loop_seconds) > 0.08:
            print("  FAIL: a loop pass measured %.3f s, expected %.1f" % (g, loop_seconds))
            ok = False

raise SystemExit(0 if ok else 1)
PYA
python3 "$TMP/analyse.py" "$TMP/out.wav" "$LOOP_SECONDS"
rc=$?
[ "$rc" = "0" ] && echo "audio_loop_check: PASS — audio loops with the arrangement, without drift" \
                || { echo "audio_loop_check: FAIL"; exit 1; }
