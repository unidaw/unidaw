#!/usr/bin/env bash
# EVENTS AT THE HEAD OF A RENDER MUST KEEP THEIR SPACING, and must not be summed on top of
# each other.
#
# THE DEFECT THIS PINS. The engine used to map its sample clock onto a separate "plugin
# timeline" by subtracting the pipeline depth, (numBlocks-1)*blockSize. Below that value the
# subtraction has no answer, so it returned 0 — and it was applied to EVERY EVENT STAMP. The
# result: every event in the first (numBlocks-1) blocks collapsed onto its block's first
# sample. Not dropped, not late: STACKED.
#
# MEASURED with three notes written at samples 0, 400 and 900 (numBlocks=3, blockSize=512, so
# the window is 1024 samples):
#
#     before   pulses: 2  -> [(0, 32767), (512, 23170)]
#     after    pulses: 3  -> [(0, 23170), (400, 23170), (900, 23170)]
#
# The first "before" pulse is at FULL SCALE because two notes landed on the same sample and
# the Identity fixture adds. That is the audible half: the head of a bounce is not just
# mistimed, it is louder, and it clips.
#
# WHY IT WAS INVISIBLE. The subtraction was applied to the events AND to the block window the
# host compares them against, so the constant cancelled and the mapping did nothing anywhere
# else — the master host has never used it at all, passing its engine sample as both
# arguments. And no fixture in the suite put a note inside the window: phase3's pulse tests
# start at beat 1, which at 120bpm/44.1kHz is sample 22050, twenty times past it. A defect
# that only exists in the first 23ms needs a fixture aimed at the first 23ms.
#
# TWO PIPELINE DEPTHS. The window is (numBlocks-1)*blockSize, so it MOVES. Running at 3 and at
# 8 means a regression cannot hide by being reintroduced at a depth this check does not use.
#
# THE INSTRUMENT IS THE MEASUREMENT. Identity emits a 10-sample pulse at the note's offset and
# ADDS it, so collapsed events show up twice over: as a missing pulse and as a doubled one.
# Rendered offline: no device, no wall clock, byte-deterministic.
#   tools/pdc_head_event_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000
SR=44100
BLOCK=512

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
IDENTITY="$BUILD/identity_plugin_artefacts/VST3/Identity.vst3"
[ -d "$IDENTITY" ] || { echo "build identity_plugin_VST3 first"; exit 2; }

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

# THE TARGETS ARE SAMPLES, converted to nanoticks here rather than written as constants: at
# another tempo or rate a hard-coded nanotick would silently stop being inside the window and
# the check would pass by measuring an ordinary position. 0, 400 and 900 are inside 1024 (the
# window at numBlocks=3) and span two blocks, so a per-block collapse and a whole-window
# collapse are both visible.
TARGETS="0 400 900"
python3 - "$TMP" "$Q" "$IDENTITY" "$SR" "$TARGETS" <<'PY'
import json, os, sys
tmp, Q, ident, sr = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
targets = [int(x) for x in sys.argv[5].split()]
spt = (60.0 / 120.0) * sr / Q
ticks = [int(round(s / spt)) for s in targets]
print("  notes at samples %s -> nanoticks %s" % (targets, ticks))
# DISTINCT PITCHES so the engine cannot merge them as one retriggered note, and distinct
# columns so nothing in the tracker layer coalesces them either.
notes = [{"nanotick": t, "duration": 120000, "pitch": 60 + i, "velocity": 120,
          "column": i, "note_id": i + 1} for i, t in enumerate(ticks)]
BAR = Q * 4
clip = {"id": 1, "name": "c", "length": BAR * 4, "kind": "symbolic", "notes": notes}
tr = {"track_id": 0, "name": "N",
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "device_chain": [{"device_id": 1, "kind": "vst_instrument", "capability_mask": 5,
                        "patcher_node_id": 4294967295, "host_slot_index": 4294967294,
                        "bypass": False,
                        "vst_ref": {"vendor": "daw", "name": "Identity", "path": ident,
                                    "uid16": ""}}],
      "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 4,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "pdchead"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]},
          open(os.path.join(tmp, "pdchead.uniproj.json"), "w"))
PY

for NB in 3 8; do
  WINDOW=$(( (NB - 1) * BLOCK ))
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/pdch_${NB}_$$" DAW_PROJECT_DIR="$TMP" \
      DAW_ENGINE_NUM_BLOCKS=$NB \
      ./daw_engine --project pdchead --render "out$NB" --run-seconds 2 \
      --block-size $BLOCK --sample-rate $SR >"$TMP/eng$NB.log" 2>&1 ) \
    || fail "the render exited non-zero at numBlocks=$NB — see $TMP/eng$NB.log"
  [ -s "$TMP/out$NB.wav" ] || fail "the render wrote no output at numBlocks=$NB"

  python3 - "$TMP/out$NB.wav" "$TARGETS" "$NB" "$WINDOW" <<'PY' || exit 1
import sys, wave, struct
path, targets, nb, window = sys.argv[1], [int(x) for x in sys.argv[2].split()], sys.argv[3], sys.argv[4]
w = wave.open(path, 'rb'); n, ch = w.getnframes(), w.getnchannels()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
mono = [abs(sum(s[i*ch:(i+1)*ch]) / ch) for i in range(n)]
head = mono[:4000]
peak = max(head) if head else 0
if peak == 0:
    print("  FAIL: numBlocks=%s rendered SILENCE at the head — the notes never sounded, so"
          "\n        the timing assertion below would be vacuous." % nb)
    raise SystemExit(1)
thr = peak * 0.25
i, pulses = 0, []
while i < len(head):
    if head[i] > thr:
        j = i
        while j < len(head) and head[j] > thr:
            j += 1
        pulses.append((i, int(max(head[i:j]))))
        i = j
    else:
        i += 1
print("  numBlocks=%s (window %s samples): %d pulse(s) -> %s"
      % (nb, window, len(pulses), pulses[:6]))

if len(pulses) != len(targets):
    print("\n  FAIL: %d notes were written inside the first %s samples and %d pulse(s) came out."
          "\n        %s" % (len(targets), window, len(pulses), pulses[:6]))
    print("""
        This is the pipeline-depth clamp. The engine mapped its sample clock to a plugin
        timeline by subtracting (numBlocks-1)*blockSize, which has no answer below that
        value, so every event in the first blocks was stamped 0 and they arrived stacked on
        the block boundary rather than where they were written.

        The subtraction cancels against the block window the host compares events against, so
        it never did anything except here. The plugin timeline is the engine timeline; see
        apps/latency_manager.h.""")
    raise SystemExit(1)

# WHERE, not just how many. Equal counts with wrong positions is the failure mode a count
# alone cannot see — two notes summed onto one sample plus a stray pulse elsewhere would
# still total three.
for want, (got, amp) in zip(targets, pulses):
    if abs(got - want) > 2:
        print("\n  FAIL: a note written at sample %d first sounds at sample %d." % (want, got))
        raise SystemExit(1)

# AND HOW LOUD. Collapsed events SUM, because Identity adds — so a stacked pair reads as one
# pulse at up to full scale. Comparing every pulse against the quietest catches that even if
# the positions happened to survive.
lo = min(a for _, a in pulses)
for pos, amp in pulses:
    if amp > lo * 1.35:
        print("\n  FAIL: the pulse at sample %d peaks at %d against a baseline of %d — more than"
              "\n        one note landed on it. The Identity fixture ADDS, so events collapsed onto"
              "\n        the same sample come back as one louder pulse, and near full scale they"
              "\n        CLIP. The head of a bounce is not only mistimed, it is distorted." % (pos, amp, lo))
        raise SystemExit(1)
PY
done

echo "pdc_head_event_check: PASS — notes at samples $TARGETS keep their spacing and their level" \
     "at pipeline depths 3 and 8, where the clamp used to stack them on the block boundary"
