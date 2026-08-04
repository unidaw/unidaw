#!/usr/bin/env bash
# EDITING A SAMPLER WHILE IT IS SOUNDING MUST NOT FREE WHAT THE VOICES ARE READING.
#
# A SamplerVoice borrows: its envelopes, its decoded sample planes and its mip-map are all RAW
# pointers into a SamplerRender it does not own. sampler_voice.h says so outright — "a voice never
# owns one and never frees one". Replacing that snapshot therefore has to keep the outgoing one
# alive until the voices reading it are done, and for a long time it did not: setSnapshot dropped
# the last reference and freed it under sounding notes.
#
# Every sampler edit goes through refreshSamplerForTrack, so ANY edit during playback could do it
# — a filter change, an envelope tweak, a slice drag, a slider a UI sends on every frame.
# AddressSanitizer named it: heap-use-after-free, READ of size 8, on a render-pool worker.
#
# WHY THIS CHECK HAMMERS INSTEAD OF EDITING ONCE. A single edit crashed about one run in ten. At
# that rate a check is worse than nothing: it goes green on a broken engine nine times out of ten
# and then "fails intermittently" once, which reads as flakiness and gets rerun until it passes.
# Forty edit triples against sounding voices took it to 3/3 — a rate a check can actually assert
# on. The fix takes it to 0/5.
#
# The lesson generalises past this bug: when a defect is rare, the check's job is to make it
# COMMON, not to observe patiently. A test that reproduces a real crash one run in ten is a test
# that teaches people to ignore it.
#
# TWO PROPERTIES, and the second is not decoration:
#   SURVIVES   the engine exits 0 through a storm of edits over sounding voices
#   PLAYS      the take has audio in it. An engine that crashed nothing because it produced
#              nothing would satisfy the first property perfectly
#
# Rendered OFFLINE. No audio device needed.
#   tools/sampler_edit_while_playing_check.sh [rounds]      (default 2)
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# DAW_BUILD_DIR SO THE ADVICE BELOW IS ACTIONABLE. This file tells you to rebuild with
# -fsanitize=address and re-run, and until this line that meant editing the script. Now:
#
#   cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#     -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1" \
#     -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" -DDAW_BUILD_PATCHER_RUST=ON
#   cmake --build build-asan --target daw_engine juce_host_process -j8
#   DAW_BUILD_DIR=$PWD/build-asan ASAN_OPTIONS=detect_leaks=0 \
#     bash tools/sampler_edit_while_playing_check.sh 10
#
# RESULT ON 2026-08-02, recorded so nobody repeats it: 1200 edits over sounding voices under
# AddressSanitizer, CLEAN. That matters because ASan catches a use-after-free at the moment of
# the bad ACCESS rather than waiting for a segfault, so it is far more sensitive than this check
# is on its own — and this check DID catch a real SIGSEGV in a full ctest run the night before.
#
# So the residual crash is NOT on the snapshot-lifetime path this fixture exercises. It is
# somewhere else, or it needs conditions this fixture does not create. Naming that is worth more
# than the 1200 edits: the obvious next step has been taken and did not find it.
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
ROUNDS="${1:-2}"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
# KEEP THE EVIDENCE WHEN IT FAILS, then clean up exactly as before. The failure messages in these
# checks point at logs inside $TMP, and cleanup() removes $TMP — so the one run whose log you need
# is the one run that deletes it. This wraps the existing cleanup rather than editing it: cleanup
# still runs, still stops engines, still removes the directory.
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

# THE CRASH DELETED ITS OWN EVIDENCE, which is why the one that happened on 2026-08-01 is
# recorded and not explained.
#
# This check caught a real SIGSEGV in a full ctest run — round 1 of 2, the engine dead of signal
# 11 — and then the EXIT trap removed the project, the render and every engine log on the way
# out. It has not reproduced since: 36 rounds sequentially and 12 across three concurrent runs,
# all clean. So the residual path is rarer than the one the original fix closed, and the next
# occurrence is the only chance to see it.
#
# Copied, not moved, and only on a crash — a passing run leaves nothing behind. Two other checks
# in this repo learned the same lesson the same way (#91's module_check and #102's renders): an
# intermittent failure that erases its inputs stays unexplained for as long as it lives.
KEEP="${TMPDIR:-/tmp}/sampler_edit_crash.$$"
preserve() {
  mkdir -p "$KEEP" 2>/dev/null || return 0
  cp -R "$TMP"/. "$KEEP/" 2>/dev/null || true
  echo "  evidence kept: $KEEP (the project, every engine log, and any render that got written)"
  echo "  next step: rebuild with -fsanitize=address and re-run — the original defect here was"
  echo "  a heap-use-after-free READ of size 8 on a render-pool worker, and ASan named it"
}
fail() { echo "  FAIL: $*"; preserve; exit 1; }

python3 - "$TMP/s.wav" <<'PY'
import sys, wave, struct
sr = 48000
period = sr // 220
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(
    struct.pack('<h', int(12000 * (2.0 * (i % period) / period - 1.0))) for i in range(sr)))
w.close()
PY

python3 - "$TMP/e.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
# LONG, OVERLAPPING NOTES. The bug is about voices that are STILL SOUNDING when an edit lands, so
# the fixture has to keep several alive at once: short one-shots that have already finished cannot
# be reading anything, and a fixture of those would pass against the broken engine.
notes = [{"nanotick": i * (Q // 2), "duration": Q * 6, "pitch": 48 + (i % 12) * 2,
          "velocity": 100, "column": i % 4, "note_id": i + 1} for i in range(48)]
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 32, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "s.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 800,
                                 "resonance_milli": 0, "next_modulator_id": 1,
                                 "modulators": []}],
                   "slots": [{"id": 1, "name": "saw", "source_local_id": 1, "slice_id": 0,
                              "start_frame": 0, "end_frame": 0,
                              "loop_start_frame": 0, "loop_end_frame": 0,
                              "loop_xfade_frames": 0, "loop_mode": 1, "sustain_loop": 1,
                              "key_low": 0, "key_high": 127, "root_key": 60,
                              "pitch_track_milli": 1000, "tune_cents": 0,
                              "vel_low": 0, "vel_high": 127, "layer_group": 0,
                              "select_mode": 0, "gate": 1, "reverse": 0,
                              "gain_millibels": -600, "pan_thousandths": 0, "voice_group": 0,
                              "nna": 1, "polyphony": 0, "choke_fade_us": 3000,
                              "mod_set_id": 1, "output_stem": 0, "quality": 2}]}}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 4,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "e"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR * 4, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY

CRASHED=0
for round in $(seq 1 "$ROUNDS"); do
  export DAW_UI_SHM_NAME="/edplay_${$}_$round" DAW_PROJECT_DIR="$TMP"
  ( cd "$BUILD" && ./daw_engine --project e --render "take$round" --run-seconds 10 \
      --block-size 256 >"$TMP/r$round.log" 2>&1 ) &
  ENG=$!
  for _ in $(seq 1 120); do
    grep -q 'starting threads' "$TMP/r$round.log" 2>/dev/null && break
    sleep 0.25
  done
  # THE STORM. Every one of these goes through refreshSamplerForTrack and replaces the snapshot
  # the sounding voices are reading from.
  for i in $(seq 1 40); do
    "$CLI" do sampler-filter --track 0 --type lp12 --cutoff $((100 + i * 20)) >/dev/null 2>&1
    "$CLI" do sampler-env --track 0 --target cutoff --attack $((10000 + i * 1000)) \
      --decay 200000 --sustain 500 --release 100000 >/dev/null 2>&1
    "$CLI" do sampler-filter --track 0 --type off >/dev/null 2>&1
    kill -0 "$ENG" 2>/dev/null || break
  done
  wait "$ENG"; RC=$?; ENG=""
  if [ "$RC" -ne 0 ]; then
    CRASHED=$((CRASHED + 1))
    echo "  round $round: engine exited $RC $([ "$RC" -eq 139 ] && echo '(SIGSEGV)')"
  else
    echo "  round $round: survived 120 edits over sounding voices"
  fi
done

# ---- SURVIVES.
[ "$CRASHED" -eq 0 ] || \
  fail "the engine died in $CRASHED of $ROUNDS rounds. A sampler edit replaces the snapshot that
        sounding voices hold raw pointers into — their envelopes, their sample planes, their
        mip-map — so the retired snapshot has to outlive them. Run it under
        -fsanitize=address to see the use-after-free directly"

# ---- PLAYS. An engine that produced silence would have survived perfectly.
LAST="$TMP/take${ROUNDS}.wav"
[ -s "$LAST" ] || fail "the render wrote no output, so nothing was sounding when the edits landed
        and this check proved nothing at all"
PEAK="$(python3 - "$LAST" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n = w.getnchannels(), w.getnframes()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
print(max((abs(v) for v in s), default=0))
PY
)"
[ "${PEAK:-0}" -gt 500 ] || \
  fail "the take is silent (peak ${PEAK:-0}). No voices were sounding, so no voice was reading the
        snapshot an edit retired, and surviving that proves nothing"
echo "  the take has audio in it (peak $PEAK)"

echo "sampler_edit_while_playing_check: PASS — $ROUNDS round(s) of edits over sounding voices"
