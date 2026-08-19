#!/usr/bin/env bash
# A STEREO CLIP PLAYED AS MONO WHILE ITS WAVEFORM DREW STEREO.
#
# `decodeAudioFileMono` read every channel, built the DISPLAY pyramid from the multi-channel
# buffer — correctly, per channel — and then averaged the channels into one mono buffer and threw
# the rest away. `renderAudioRegionBlock` was mono-in/mono-out, and the RT call site constant-power
# panned that mono into the master's front L/R.
#
# So the picture above the clip was honest and what came out of the speakers was a downmix. That
# is a display/playback divergence, which is the class this codebase spends most of its effort
# removing — and it survived because EVERY audio fixture in the suite is mono: a mono sine
# (audio_clip_playback), a mono click (tempo_map_audio), a mono loop (audio_loop). The fixtures
# decided which bugs were findable, again.
#
# THE FIXTURE IS HARD-PANNED ON PURPOSE. Left carries a tone, right carries silence, then they
# swap. A centred stereo file whose channels were IDENTICAL would sound the same downmixed, so it
# could not tell a working engine from the broken one. These channels are maximally different, and
# a downmix collapses them to a constant half-amplitude tone in both outputs — which is exactly
# what this asserts must NOT happen.
#
# FOUR PROPERTIES:
#   SEPARATE   left-only content reaches the LEFT output and not the right, and vice versa
#   NOT MIXED  neither output carries the other's content — the downmix signature
#   BALANCE    pan on a STEREO source attenuates one side; it does not reposition. At centre both
#              sides pass at unity, which is the only setting that leaves the file as recorded.
#              (A MONO source still gets constant-power placement — two different meanings for one
#              control, and conflating them makes a centred stereo clip narrower than the file.)
#   MONO STILL WORKS  a mono source still feeds both outputs through the pan law
#
# Rendered OFFLINE: this is arithmetic about which samples land in which channel, and a render is
# exact where a device capture has a start transient. No audio device needed.
#   tools/stereo_audio_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000
BAR=$((Q * 4))

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

# A stereo file: 0.5s of tone LEFT only, then 0.5s of tone RIGHT only. Hard-panned so a downmix is
# unmistakable — it would put half-amplitude tone in BOTH channels for the whole second.
python3 - "$TMP/lr.wav" <<'PY'
import sys, wave, struct, math
sr = 44100
half = sr // 2
frames = []
for i in range(half):                      # left tone, right silent
    v = int(24000 * math.sin(2 * math.pi * 440.0 * i / sr))
    frames.append(struct.pack('<hh', v, 0))
for i in range(half):                      # right tone, left silent
    v = int(24000 * math.sin(2 * math.pi * 660.0 * i / sr))
    frames.append(struct.pack('<hh', 0, v))
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(frames)); w.close()
PY

# A mono file, so the mono path is asserted too rather than assumed unchanged.
python3 - "$TMP/mono.wav" <<'PY'
import sys, wave, struct, math
sr = 44100
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(24000 * math.sin(2 * math.pi * 440.0 * i / sr)))
                       for i in range(sr)))
w.close()
PY

make_project() {  # make_project <name> <wav> <pan>
  python3 - "$TMP/$1.uniproj.json" "$2" "$3" "$Q" <<'PY'
import json, sys
out, wav, pan, Q = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4])
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
clip = {"id": 1, "name": "src", "length": BAR, "kind": "audio",
        "audio": {"source_path": wav, "source_start_frame": 0,
                  "gain_db": 0.0, "fade_in": 0, "fade_out": 0}}
tr = {"track_id": 0, "name": "A", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": pan, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "s"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY
}

render() {  # render <project> -> <project>.wav
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/stchk_$$_$1" \
      ./daw_engine --project "$1" --render "$1" --sample-rate 44100 --run-seconds 2 \
      >"$TMP/$1.log" 2>&1 ) \
    || fail "the '$1' render exited non-zero — see $TMP/$1.log"
  [ -s "$TMP/$1.wav" ] || fail "the '$1' render wrote no output"
}

# Peak of each output channel over the FIRST and SECOND half-second, which is where the fixture
# puts its left-only and right-only content.
halves() {  # halves <wav> -> "L1 R1 L2 R2" as milli-units
  python3 - "$1" <<'PYH'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
half = sr // 2
def peak(c, a, b):
    lo, hi = max(0, a), min(n, b)
    vals = [abs(s[i * ch + c]) for i in range(lo, hi)] if ch > c else [0]
    return int(1000 * (max(vals) if vals else 0) / 32768.0)
print(peak(0, 0, half), peak(1, 0, half), peak(0, half, 2 * half), peak(1, half, 2 * half))
PYH
}

make_project centred "$TMP/lr.wav" 0.0
render centred
read -r L1 R1 L2 R2 <<<"$(halves "$TMP/centred.wav")"
echo "  centred stereo: first half L=$L1 R=$R1 | second half L=$L2 R=$R2"

# ---- SEPARATE. The left-only half must reach the left output.
[ "$L1" -gt 300 ] || fail "the left-only half produced almost nothing on the left output ($L1)"
[ "$R2" -gt 300 ] || fail "the right-only half produced almost nothing on the right output ($R2)"

# ---- NOT MIXED. This is the downmix signature and the whole point of the check: averaging the
# channels puts half-amplitude tone in BOTH outputs for BOTH halves.
[ "$R1" -lt 100 ] || \
  fail "the LEFT-only half is coming out of the RIGHT output at $R1. That is the downmix: the
        decoder averaged the channels and the renderer played one mono buffer into both sides,
        while the waveform drawn above the clip was per-channel and correct"
[ "$L2" -lt 100 ] || \
  fail "the RIGHT-only half is coming out of the LEFT output at $L2 — the same downmix, the
        other way round"
echo "  separate: each side's content reaches its own output and not the other"

# ---- BALANCE, not repositioning. Hard left on a STEREO source must silence the right output and
# leave the left at unity — not swing a phantom image.
make_project hardleft "$TMP/lr.wav" -1.0
render hardleft
read -r HL1 HR1 HL2 HR2 <<<"$(halves "$TMP/hardleft.wav")"
[ "$HR2" -lt 100 ] || \
  fail "panned hard left, the right output still carries the right channel at $HR2 — pan on a
        stereo source is a BALANCE, so hard left must silence the right side"
[ "$HL1" -ge $((L1 - 20)) ] || \
  fail "panned hard left, the LEFT side dropped from $L1 to $HL1. A balance attenuates the side
        you turn away from and leaves the other alone; if both moved, the stereo source is being
        treated as a point source and a centred clip will come out narrower than the file"
echo "  balance: hard left silences the right and leaves the left at unity ($HL1 vs $L1)"

# ---- AND THE MONO PATH IS UNCHANGED. A mono source is one signal PLACED by the pan, so centre
# puts it in both outputs at constant power. Asserted because this change could easily have made
# a mono clip come out of one speaker.
make_project monocentre "$TMP/mono.wav" 0.0
render monocentre
read -r ML1 MR1 ML2 MR2 <<<"$(halves "$TMP/monocentre.wav")"
[ "$ML1" -gt 300 ] && [ "$MR1" -gt 300 ] || \
  fail "a centred MONO clip should reach both outputs, got L=$ML1 R=$MR1"
DIFF=$(( ML1 > MR1 ? ML1 - MR1 : MR1 - ML1 ))
[ "$DIFF" -lt 60 ] || \
  fail "a centred mono clip is lopsided (L=$ML1 R=$MR1) — constant-power placement should put it
        equally in both"
echo "  mono: a centred mono clip still reaches both outputs equally (L=$ML1 R=$MR1)"

echo "stereo_audio_check: PASS — a stereo clip plays in stereo, and pan means the right thing for each"
