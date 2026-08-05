#!/usr/bin/env bash
# EVERY NOTE LANDS ON ITS OWN SAMPLE, INCLUDING AT THE VERY OPENING OF A RENDER.
#
# THIS CHECK USED TO BE PART INVERTED. Two of its assertions pinned behaviour that was WRONG:
# apps/latency_manager.h mapped engine samples onto a plugin timeline by subtracting the pipeline
# depth and clamping at zero, so every event in the opening (numBlocks-1)*blockSize samples
# collapsed onto the same instant. An opening chord or a fast run was squashed flat, once, at the
# start of playback — and never on a loop, which is why it read as imagination rather than a bug.
# It was pinned rather than fixed because the fix looked like an owner's call between three
# options that traded off against each other.
#
# IT WAS NOT. Measured 2026-08-05: the host windows a block's events on the same start value the
# engine hands it, so the subtraction was applied to both sides and cancelled — it changed nothing
# anywhere except where it could not be applied at all. The master host had never used it, passing
# its engine sample as both arguments. So the plugin timeline is the engine timeline now, the
# clamped mapping survives only as the visual playhead (apps/latency_manager.h), and the two
# inverted blocks that lived here have been deleted per their own retirement instructions.
#
# WHAT REPLACED THEM is directly below: the exact-placement table now runs THROUGH the old window
# instead of starting past it, and the two-note case asserts the opening keeps the same spacing the
# loop pass does. That second one is the assertion that states the harm the defect actually did —
# not "each note is early" but "they were no longer distinguishable from each other".
#
# tools/pdc_head_event_check.sh covers the same rule through a REAL hosted VST3 and the IPC that
# carries the stamps; this one uses the in-process fake, so a regression in either the engine-side
# arithmetic or the wire is caught by one of them.
#
# WHY THIS COULD NOT BE WRITTEN BEFORE: the placements are sample counts, so the render has to
# happen at a KNOWN rate. Until `--sample-rate` existed the offline render took whatever the
# default output device reported, and connecting headphones would have moved every number in the
# table — the exact coupling that made sampler_vintage fail a correct engine.
#
#   tools/pdc_window_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000
RATE=44100
BLOCK=512

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
TMP="$(mktemp -d)"
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
fail() { echo "  FAIL: $*"; exit 1; }

# One note per render, at a stated SAMPLE. 120 bpm at 44100 makes a quarter 22050 samples, so the
# nanotick is sample * Q / 22050. DAW_USE_FAKE_IDENTITY makes the instrument write a 10-sample
# pulse at the event's offset, so the FIRST NON-ZERO SAMPLE of the take is the placement — no
# envelope, no attack, nothing to interpret.
mk() {  # mk <name> <sample> [second-sample]
  python3 - "$TMP/$1.uniproj.json" "$2" "${3:-}" <<'PY'
import json, sys
out, first = sys.argv[1], int(sys.argv[2])
second = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
Q = 960000; DIRECT = 4294967294
tick = lambda s: round(s * Q / 22050)
notes = [{"nanotick": tick(first), "duration": Q // 8, "pitch": 60, "velocity": 100,
          "column": 0, "note_id": 1}]
if second is not None:
    notes.append({"nanotick": tick(second), "duration": Q // 8, "pitch": 72, "velocity": 100,
                  "column": 1, "note_id": 2})
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
dev = {"device_id": 1, "kind": "vst_instrument", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": DIRECT, "bypass": False,
       "vst_ref": {"vendor": "", "name": "identity", "path": "", "uid16": ""}}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": 4 * Q,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "n"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "c", "length": 4 * Q, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY
  ( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_PROJECT_DIR="$TMP" \
      DAW_UI_SHM_NAME="/pdcwin_${1}_$$" \
      ./daw_engine --project "$1" --render "$1" --run-seconds 3 \
      --block-size "$BLOCK" --sample-rate "$RATE" >"$TMP/$1.log" 2>&1 ) \
    || fail "the '$1' render exited non-zero — see $TMP/$1.log"
  [ -s "$TMP/$1.wav" ] || fail "the '$1' render wrote no audio"
}

pulses() {  # pulses <name> -> the start sample of each burst, space separated
  python3 - "$TMP/$1.wav" <<'PY'
import struct, sys, wave
w = wave.open(sys.argv[1]); ch = w.getnchannels(); n = w.getnframes()
d = struct.unpack('<%dh' % (n * ch), w.readframes(n)); mono = d[::ch]
starts, prev = [], 0
for i, v in enumerate(mono):
    if v != 0 and prev == 0:
        starts.append(i)
    prev = v
print(' '.join(str(s) for s in starts))
PY
}

# ---- EXACT, ACROSS THE WHOLE RANGE. 86, 344 and 600 are inside the old compensation window
# (1024 samples at numBlocks=3, blockSize=512) and used to collapse onto a block boundary; 600 in
# particular was pinned here for years as "short by 88". They are first in the list deliberately:
# if the clamp ever comes back, the check fails on the very sample it used to excuse.
for S in 86 344 600 1102 2297 4096; do
  mk "exact$S" "$S"
  GOT="$(pulses "exact$S" | awk '{print $1}')"
  [ "$GOT" = "$S" ] || \
    fail "a note at sample $S rendered at ${GOT:-nothing}, and every note lands on its own sample.

        If \$S is under 1024 this is the pipeline-depth clamp returning: the engine stamped every
        event in the opening (numBlocks-1)*blockSize samples with 0, so they arrived stacked on a
        block boundary instead of where they were written. That was pinned here as a known defect
        until 2026-08-05 and is fixed — see apps/latency_manager.h. Above 1024 it is an ordinary
        scheduling regression."
done
echo "  exact across the range: 86, 344, 600, 1102, 2297, 4096 all land on their own sample"

# ---- RELATIVE TIMING AT THE OPENING, against a reference the render carries itself.
#
# TWO notes, at different samples, both inside the old window. This is the assertion that states
# what the defect actually cost: not "each note is early" but "they were no longer distinguishable
# from each other", which is what a squashed opening chord is.
#
# THE RENDER IS ITS OWN CONTROL, so nothing here is a hardcoded expectation. The clip loops every
# 4 quarters — 88200 samples at this rate — and the loop pass was always past the compensation
# window, so the same two notes appear there with their spacing intact. The opening must now match
# it. When this check was inverted, the opening had ONE pulse and the loop pass had two 258 apart;
# that difference was the defect, and its absence is the fix.
mk "collapse" 86 344
STARTS="$(pulses collapse)"
python3 - "$STARTS" <<'PYS' || fail "see the message above"
import sys
LOOP = 88200          # 4 quarters at 120 bpm, 44100 Hz
WANT_GAP = 344 - 86   # what the two notes were authored to be apart
starts = [int(x) for x in sys.argv[1].split()]
first = [s for s in starts if s < LOOP]
later = [s for s in starts if s >= LOOP]
if len(later) < 2:
    print(f"  FAIL: the loop pass produced {len(later)} pulse(s) ({later}); this check needs the")
    print( "        second pass as its reference, so something changed about looping rather than")
    print( "        about compensation")
    raise SystemExit(1)
gap_later = later[1] - later[0]
if gap_later != WANT_GAP:
    print(f"  FAIL: on the loop pass the two notes are {gap_later} samples apart, not {WANT_GAP}.")
    print( "        The reference itself is wrong, so nothing below can be trusted")
    raise SystemExit(1)
if len(first) != 2:
    print(f"  FAIL: the opening pass has {len(first)} pulse(s) at {first}, and it should have 2 —")
    print(f"        the SAME two notes on the loop pass are {gap_later} samples apart.")
    print( "")
    print( "        ONE pulse is the pipeline-depth clamp coming back: the opening window mapped")
    print( "        to a plugin timeline by subtracting (numBlocks-1)*blockSize, which has no")
    print( "        answer below itself, so every event there was stamped 0 and they arrived")
    print( "        stacked. See apps/latency_manager.h — the plugin timeline is the engine")
    print( "        timeline, and only the visual playhead still subtracts.")
    raise SystemExit(1)
gap_first = first[1] - first[0]
if gap_first != gap_later:
    print(f"  FAIL: the opening pass has the two notes {gap_first} samples apart and the loop pass")
    print(f"        has them {gap_later} apart. The same two notes must be spaced the same way")
    print( "        wherever they are played; the opening is not a special case any more.")
    raise SystemExit(1)
print(f"  opening pass: two pulses at {first}, {gap_first} apart — the same spacing the loop pass")
print( "    has, where the clamp used to leave exactly one")
PYS

# ---- AND THE EDGE, which used to move rather than collapse: a note just inside the window was
# shifted back to the block boundary. It is in the exact table above now, so this only records
# what the old number was, for anyone reading a git blame.
mk "edge" 600
EDGE="$(pulses edge | awk '{print $1}')"
[ "$EDGE" = "600" ] || fail "a note at sample 600 rendered at ${EDGE:-nothing}. This one was
        pinned for a long time as 'short by 88' — the clamp pulling it back to the block
        boundary — so a non-exact value here is that defect returning"
echo "  a note at sample 600 lands at 600, where the clamp used to leave it 88 samples short"

echo "pdc_window_check: PASS — exact across the whole range, and the opening keeps the same" \
     "spacing the loop pass does; the inverted blocks retired 2026-08-05"
