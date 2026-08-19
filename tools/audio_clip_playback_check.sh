#!/usr/bin/env bash
# End-to-end check that a placed AUDIO CLIP actually plays (M4). Renders the
# engine's master output to a wav via the capture tap and asserts it with
# perceptual.py. Needs a real audio device (runs the engine in NON-test mode) and
# the C++ + daw-cli targets built. Not a headless-CI test — a dev/agent check.
#
#   tools/audio_clip_playback_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"
SINE="$TMP/sine.wav"
TAKE="$TMP/take.wav"
SHM="/audio_clip_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

# A 0.6 s 440 Hz sine as the clip source.
python3 - "$SINE" <<'PY'
import sys, wave, struct, math
sr, dur, f = 44100, 0.6, 440.0
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(0.6*32767*math.sin(2*math.pi*f*i/sr)))
                       for i in range(int(sr*dur))))
w.close()
PY

# A project: one audio clip placed at bar 0 on track 0.
cat > "$TMP/audioplay.uniproj.json" <<EOF
{ "schema_version": 4,
  "meta": { "name": "audioplay", "created_utc": 0, "modified_utc": 0 },
  "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
  "harmony_timeline": [],
  "clips": [ { "id": 1, "name": "sine", "length": $((4*Q)), "kind": "audio",
    "audio": { "source_path": "$SINE", "source_start_frame": 0, "gain_db": 0.0,
               "fade_in": 0, "fade_out": 0 } } ],
  "tracks": [ { "track_id": 0, "name": "Audio", "harmony_quantize": 0, "lines_per_beat": 4,
    "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
    "device_chain": [], "mod_links": [],
    "placements": [ { "clip_id": 1, "at": 0, "length": $((4*Q)),
                      "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

# RENDERED OFFLINE. "Does an audio region reach the mix" is arithmetic plus a decode; no sound
# card is involved in the answer, and a render has no start transient and no capture ring to
# drop the beginning. The realtime pull path is covered by offline_render_check (which pins a
# render against a device capture of the same fixture) and by the checks that stay on hardware.
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project audioplay --render take --sample-rate 44100 --run-seconds 6 \
    >"$TMP/engine.log" 2>&1 ) \
  || { echo "FAIL: the render exited non-zero — see $TMP/engine.log"; exit 1; }
[ -s "$TAKE" ] || { echo "FAIL: the render wrote no output"; exit 1; }

echo "--- perceptual ---"
python3 "$ROOT/tools/perceptual.py" --expect-audio "$TAKE"
echo "audio_clip_playback_check: PASS"
rm -rf "$TMP"
