#!/usr/bin/env bash
# Checks 4b: a VST EFFECT on the MASTER SUM actually processes the mix, out of process,
# one block behind the audio callback (design option B2) — and that a project with NO
# master effect is unaffected.
#
# The stimulus is an audio CLIP (rendered in the callback itself), so the sum is
# deterministic and non-zero the moment the transport rolls — no instrument, no plugin
# timing to race. The master effect is the repo's own Identity fixture in pass-through
# mode (DAW_IDENTITY_PASSTHRU=1 makes it a unity-gain insert effect), so the check needs
# no third-party plugin installed.
#
# PASS means: with a unity-gain effect on the master, the captured output still carries
# the audio at essentially the same level as with no master effect at all. If the master
# host were failing (wrong plugin, no audio-in, dead handshake) the capture would be
# SILENT — which is exactly how this path failed while it was being built.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/master_fx_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
IDENTITY="$BUILD/identity_plugin_artefacts/VST3/Identity.vst3"
Q=960000
MASTER_ID=4294901760

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }
[ -d "$IDENTITY" ] || { echo "build identity_plugin_VST3 first"; exit 2; }

TMP="$(mktemp -d)"
python3 - "$TMP/sine.wav" <<'PY'
import sys, wave, struct, math
sr, dur, f = 44100, 8.0, 440.0
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(0.6*32767*math.sin(2*math.pi*f*i/sr)))
                       for i in range(int(sr*dur))))
w.close()
PY

MASTER_TRACK=", { \"track_id\": $MASTER_ID, \"name\": \"Master\", \"is_master\": true,
      \"mixer\": { \"gain_db\": 0.0, \"pan\": 0.0, \"mute\": false, \"solo\": false },
      \"device_chain\": [ { \"device_id\": 0, \"kind\": \"vst_effect\", \"capability_mask\": 4,
        \"patcher_node_id\": 4294967295, \"host_slot_index\": 4294967294, \"bypass\": false,
        \"vst_ref\": { \"vendor\": \"daw\", \"name\": \"Identity\", \"path\": \"$IDENTITY\", \"uid16\": \"\" } } ],
      \"mod_links\": [], \"placements\": [] }"

gen() {  # gen <name> <extra-tracks-json>
  cat > "$TMP/$1.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "$1" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [ { "id": 1, "name": "sine", "length": $((16*Q)), "kind": "audio",
    "audio": { "source_path": "$TMP/sine.wav", "source_start_frame": 0, "gain_db": 0.0,
               "fade_in": 0, "fade_out": 0 } } ],
  "tracks": [
    { "track_id": 0, "name": "Audio", "harmony_quantize": 0, "lines_per_beat": 4,
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "placements": [ { "clip_id": 1, "at": 0, "length": $((16*Q)),
                        "notes": [], "chords": [], "mutes": [] } ] }$2 ] }
EOF
}
gen nofx ""
gen withfx "$MASTER_TRACK"

run() {  # run <name>
  local shm="/master_fx_${1}_$$"
  ( cd "$BUILD" && exec env DAW_IDENTITY_PASSTHRU=1 DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" \
      DAW_CAPTURE_WAV="$TMP/$1.wav" DAW_CAPTURE_SECONDS=6 \
      ./daw_engine --run-seconds 12 >"$TMP/$1.log" 2>&1 ) &
  local eng=$!
  sleep 2.5
  DAW_UI_SHM_NAME="$shm" "$CLI" do load "$1" --force >/dev/null 2>&1 || true
  sleep 1.5
  DAW_UI_SHM_NAME="$shm" "$CLI" do play --force >/dev/null 2>&1 || true
  wait "$eng" 2>/dev/null || true
}
run nofx
sleep 1   # let the audio device settle before the next engine claims it
run withfx

ok=1
# The captures must exist at all — an engine that never got the device (or died early)
# would otherwise crash the comparison below with a confusing FileNotFoundError.
for f in nofx withfx; do
  [ -s "$TMP/$f.wav" ] || {
    echo "  FAIL (setup): $f produced no capture — engine log tail:"
    tail -3 "$TMP/$f.log" | sed 's/^/    /'
    ok=0
  }
done
# 1. The master host must have come up for the withfx project (and NOT for nofx).
if grep -q "Restarted track $MASTER_ID" "$TMP/withfx.log"; then
  echo "  master host launched for the master effect"
else
  echo "  FAIL: master host never launched"; ok=0
fi
# 2. The engine must report the one-block master latency only when engaged.
grep -q "Master FX: engaged" "$TMP/withfx.log" \
  && echo "  one-block master latency reported" \
  || { echo "  FAIL: master FX engaged but the added latency was not reported"; ok=0; }
grep -q "Master FX: engaged" "$TMP/nofx.log" \
  && { echo "  FAIL: no-master-FX project reported master FX engaged"; ok=0; } \
  || echo "  no-master-FX project takes the untouched path"

# 3. Both captures must carry the audio: a unity-gain master effect is transparent, so a
#    silent withfx capture means the master host path is broken.
python3 "$ROOT/tools/perceptual.py" --expect-audio "$TMP/nofx.wav" >/dev/null 2>&1 \
  || { echo "  FAIL: baseline (no master FX) was silent — the check itself is broken"; ok=0; }
if python3 "$ROOT/tools/perceptual.py" --expect-audio "$TMP/withfx.wav" >/dev/null 2>&1; then
  echo "  audio survives the out-of-process master effect"
else
  echo "  FAIL: audio through the master effect was SILENT"; ok=0
fi
# 4. Levels must match: unity gain in, unity gain out (10% tolerance on peak).
[ "$ok" = "1" ] && python3 - "$TMP/nofx.wav" "$TMP/withfx.wav" <<'PY' || ok=0
import sys, wave, numpy as np
def peak(p):
    w = wave.open(p, 'rb')
    d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)/32768.0
    return float(np.abs(d).max()) if d.size else 0.0
a, b = peak(sys.argv[1]), peak(sys.argv[2])
print(f"  peak: nofx={a:.3f} withfx={b:.3f}")
if a <= 0.01:
    print("  FAIL: baseline peak ~0"); raise SystemExit(1)
if abs(a-b) > 0.1*a:
    print(f"  FAIL: unity master effect changed the level by {abs(a-b)/a*100:.1f}%")
    raise SystemExit(1)
PY

# 5. SURROUND: the master host must open at the MIX's width, not a hardcoded stereo.
#    Sized at 2 it can never match a 6-channel master, and the gate then leaves the effect
#    installed, hosted and completely inaudible — which is how this shipped at first.
shm_s="/master_fx_sur_$$"
( cd "$BUILD" && exec env DAW_MASTER_CHANNELS=6 DAW_IDENTITY_PASSTHRU=1 DAW_UI_SHM_NAME="$shm_s" \
    DAW_PROJECT_DIR="$TMP" DAW_CAPTURE_WAV="$TMP/sur.wav" DAW_CAPTURE_SECONDS=6 \
    ./daw_engine --run-seconds 12 >"$TMP/sur.log" 2>&1 ) &
eng_s=$!
sleep 2.5
DAW_UI_SHM_NAME="$shm_s" "$CLI" do load withfx --force >/dev/null 2>&1 || true
sleep 1.5
DAW_UI_SHM_NAME="$shm_s" "$CLI" do play --force >/dev/null 2>&1 || true
wait "$eng_s" 2>/dev/null || true
if grep -q "Master FX: engaged" "$TMP/sur.log" && \
   ! grep -q "not using it" "$TMP/sur.log" && \
   python3 "$ROOT/tools/perceptual.py" --expect-audio "$TMP/sur.wav" >/dev/null 2>&1; then
  echo "  surround master (6ch): FX engaged and audible"
else
  echo "  FAIL: master FX did not engage on a 6-channel master"
  grep -iE "Surround master|not using it|Master FX" "$TMP/sur.log" | head -3 | sed 's/^/    /'
  ok=0
fi

rm -rf "$TMP"
[ "$ok" = "1" ] && echo "master_fx_check: PASS — the mix is processed by an out-of-process master effect, transparently" \
                || { echo "master_fx_check: FAIL"; exit 1; }
