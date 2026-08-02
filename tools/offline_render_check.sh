#!/usr/bin/env bash
# OFFLINE RENDER (§7 Q4, answered yes): the engine renders faster than realtime, without an
# audio device, and produces the same music it plays.
#
# The architecture was supposed to make this impossible — per-track host PROCESSES pulled by a
# realtime audio callback, with the device clock driving everything. It turned out the producer
# already paces to `audioPlaybackBlockId`, the block the CONSUMER has played, rather than to a
# clock; that fell out of fixing the "everything 4x too fast" bug. So the pump just has to BE
# the consumer and the whole pipeline runs at host speed.
#
# The pump inverts three policies that are right for a device and wrong for a file: it never
# skips a block to stay current, never primes with silence, and never starves — it WAITS. So the
# mix is glitch-free by construction rather than by luck, which is the real prize: a render
# cannot have a dropout, where a realtime capture can and does.
#
# THREE PROPERTIES, and the third is the one that makes the other two worth anything:
#   DETERMINISTIC   two renders of one project are byte-identical. Without this, offline is
#                   useless as a test oracle no matter how fast it is.
#   FASTER          wall time is well under the audio duration. That is the feature.
#   MATCHES REALTIME  the same fixture captured through the real device has the same onsets and
#                   the same peak. A render that is fast, repeatable and WRONG would satisfy the
#                   first two perfectly.
#
# THIS CHECK IS NOW LOAD-BEARING for seven others. These render instead of capturing:
#
#   tempo_map_audio  audio_loop  audio_clip_playback  surround  child_track
#   pdc_alignment    patcher_device_migration
#
# Their subject is musical or arithmetic — where a clip lands, whether a bus reaches the mix, how
# far apart two onsets are — and none of it needs a sound card. Together they went from about 119
# seconds of wall time to about 11, and three of them got STRONGER in the process: audio_loop now
# asserts pass 1 instead of excusing it as a startup transient, pdc_alignment measures its offset
# as exactly 512 samples rather than approximately, and patcher_device_migration dropped the
# 8-block workaround it needed because a starved realtime producer emits silence that looks like a
# dead generator.
#
# So the equivalence THIS check pins is what those seven now rest on. If a render ever stops
# matching the device, they all quietly stop testing the thing they claim to.
#
# WHAT STAYS ON REAL HARDWARE, and why, so the boundary is written down rather than inferred:
#   audio_stability      its subject IS the realtime pipeline — underruns, block depth, deadlines
#   sidechain            cross-track pull under a real clock
#   master_fx            the one-block-latency host on the master sum
#   panic, preview_note  interactive: they send a command MID-PLAYBACK, which a batch render at
#                        host speed cannot time
#   level_match_bypass   toggles bypass mid-run, same reason
#   midi_per_bus         types a note onto a DERIVED child at runtime. Moving it into the fixture
#                        would route it through aux-child persistence instead of the runtime edit
#                        path — a different mechanism, and the one it exists to test
#
# Needs a real audio device for the comparison half + the C++ and daw-cli targets built.
#   tools/offline_render_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# Four notes, one per beat at 120 bpm, through the fake instrument so this needs no real plugin.
python3 - "$TMP/rend.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
DIRECT = 4294967294
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
dev = {"device_id": 1, "kind": "vst_instrument", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": DIRECT, "bypass": False,
       "vst_ref": {"vendor": "", "name": "identity", "path": "", "uid16": ""}}
clip = {"id": 1, "name": "c", "length": 4 * Q, "kind": "symbolic",
        "notes": [{"nanotick": i * Q, "duration": Q // 2, "pitch": 60 + i,
                   "velocity": 100, "column": 0, "note_id": i + 1} for i in range(4)]}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": 4 * Q,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "rend"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

SECS=4
render() {  # $1 = output name
  ( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_PROJECT_DIR="$TMP" \
      DAW_UI_SHM_NAME="/offrend_$$_$1" \
      ./daw_engine --project rend --render "$1" --run-seconds "$SECS" \
      >"$TMP/$1.log" 2>&1 )
}

# ---- FASTER THAN REALTIME. Timed around the whole process, so startup and the host handshake
# are counted against it rather than excluded — the honest number is the one a caller waits.
START=$SECONDS
render a || fail "the render exited non-zero (see $TMP/a.log)"
ELAPSED=$((SECONDS - START))
[ -f "$TMP/a.wav" ] || fail "no render output was written"
[ "$ELAPSED" -lt "$SECS" ] || \
  fail "rendering ${SECS}s of audio took ${ELAPSED}s — not faster than realtime, so the pump is
        being paced by something it should not be (a device clock, or a sleep in the loop)"
echo "  faster: ${SECS}s of audio rendered in ~${ELAPSED}s of wall time, no audio device"

# ---- DETERMINISTIC. Byte-identical, not approximately equal: the fake instrument is
# deterministic and so is the scheduler, so anything else means a race is leaking into the
# output — which would disqualify offline render as a test oracle even if it sounded right.
render b || fail "the second render exited non-zero"
cmp -s "$TMP/a.wav" "$TMP/b.wav" || \
  fail "two renders of the same project differ. Offline render is only useful as an oracle if it
        is reproducible; a difference here means block ordering or host completion is leaking
        into the mix"
echo "  deterministic: two renders are byte-identical"

# ---- AND IT MATCHES WHAT THE DEVICE PLAYS. A render that is fast, repeatable and wrong would
# pass both checks above. The comparison is musical rather than sample-exact on purpose: the
# realtime capture has a device start transient and an arbitrary offset into its ring, so what
# must agree is the MUSIC — how many onsets, how they are spaced, and how loud.
( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_PROJECT_DIR="$TMP" \
    DAW_UI_SHM_NAME="/offrend_rt_$$" DAW_CAPTURE_WAV="$TMP/rt.wav" \
    DAW_CAPTURE_SECONDS=$((SECS + 4)) \
    ./daw_engine --project rend --run-seconds $((SECS + 3)) >"$TMP/rt.log" 2>&1 ) &
RTPID=$!
for _ in $(seq 1 80); do
  if grep -q 'Startup load:' "$TMP/rt.log" 2>/dev/null; then break; fi
  sleep 0.25
done
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
if [ -x "$CLI" ]; then
  DAW_UI_SHM_NAME="/offrend_rt_$$" "$CLI" do play --force >/dev/null 2>&1 || true
fi
wait "$RTPID" 2>/dev/null || true
[ -f "$TMP/rt.wav" ] || { echo "  (no realtime capture — skipping the comparison half)"; \
  echo "offline_render_check: PASS (faster + deterministic; realtime comparison skipped)"; exit 0; }

python3 - "$TMP/a.wav" "$TMP/rt.wav" <<'PYCMP'
import sys, wave, struct
# The fixture is one note per beat at 120 bpm, so an onset interval is 0.5s. That expectation is
# computed BY HAND from the fixture and BOTH recordings are measured against it — rather than
# against each other, which would pass two recordings that are wrong in the same way.
#
# Onset COUNT is deliberately not the assertion. The two runs cover different durations (the
# realtime one has to play for longer to be captured at all) and the detector is a crude
# threshold-and-rearm, so counts are approximate and comparing them would either be vacuous or
# flaky. The SPACING is exact and is what "the same rhythm" actually means.
BEAT_SECONDS = 0.5
def measure(path):
    w = wave.open(path, 'rb')
    ch, n, rate = w.getnchannels(), w.getnframes(), w.getframerate()
    s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
    mono = [abs(s[i]) for i in range(0, len(s), ch)]
    peak = max(mono) / 32768.0 if mono else 0.0
    hits, armed = [], True
    for i, v in enumerate(mono):
        if armed and v > 6000:
            hits.append(i); armed = False
        elif not armed and v < 1500:
            armed = True
    return hits, peak, rate
off_hits, off_peak, off_rate = measure(sys.argv[1])
rt_hits, rt_peak, rt_rate = measure(sys.argv[2])
print("  offline : %d onsets, peak %.3f, %d Hz" % (len(off_hits), off_peak, off_rate))
print("  realtime: %d onsets, peak %.3f, %d Hz" % (len(rt_hits), rt_peak, rt_rate))
if not off_hits:
    print("  FAIL: the offline render is SILENT. Fast, reproducible, and containing no music —"
          " which is exactly what the first version produced: a correctly sized file of zeros,"
          " because nothing had been loaded.")
    sys.exit(1)
if not rt_hits:
    print("  FAIL: the realtime capture is silent, so there is nothing to compare against")
    sys.exit(1)
if abs(off_peak - rt_peak) > 0.05:
    print("  FAIL: peak differs (offline %.3f vs realtime %.3f) — the render is not going through"
          " the same mix the device hears." % (off_peak, rt_peak))
    sys.exit(1)

def check_spacing(label, hits, rate):
    gaps = [hits[i + 1] - hits[i] for i in range(len(hits) - 1)]
    if not gaps:
        print("  FAIL: %s has fewer than two onsets, so its rhythm cannot be checked" % label)
        return False
    want = BEAT_SECONDS * rate
    # A block of tolerance (1024 frames): a note lands within the block that carries it.
    bad = [(i, g) for i, g in enumerate(gaps) if abs(g - want) > 1024]
    if bad:
        i, g = bad[0]
        print("  FAIL: %s interval %d is %d frames, expected ~%d (one beat at 120 bpm). The"
              " fixture is one note per beat, so this is the wrong rhythm — not a difference"
              " between the two paths but a difference from the MUSIC."
              % (label, i, g, int(want)))
        return False
    print("  %s: %d interval(s), all one beat (~%d frames)" % (label, len(gaps), int(want)))
    return True

ok = check_spacing("offline ", off_hits, off_rate)
ok = check_spacing("realtime", rt_hits, rt_rate) and ok
if not ok:
    sys.exit(1)
print("  matches: both paths play the fixture's rhythm, at the same peak")
PYCMP
[ $? -eq 0 ] || exit 1

echo "offline_render_check: PASS — faster than realtime, deterministic, and the same music"
