#!/usr/bin/env bash
# Checks LEVEL-MATCHED BYPASS (roadmap 15c): toggling an insert's bypass must compare TONE,
# not LOUDNESS. An insert that merely makes things louder should stop sounding "better".
#
# The stimulus is deliberately ARITHMETIC rather than musical: an audio clip (rendered in
# the callback, so it starts the instant the transport rolls) through the Identity fixture
# acting as an insert with a KNOWN fixed gain (DAW_FAKE_INSERT_GAIN). With a 0.5x insert:
#   - RAW bypass jumps to 2.0x the active level (the insert's attenuation disappears).
#   - LEVEL-MATCHED bypass stays at ~1.0x (the passthrough is scaled to match).
# Three earlier attempts at this used a real plugin and a live instrument and proved
# nothing — a project loaded already bypassed never measures a gain to correct with,
# bypassing the master's only effect used to disengage the whole master-FX path, and the
# instrument stimulus was timing-flaky. A known gain makes the expected number exact, and
# it immediately caught the ratio being INVERTED (bypass was made louder, not matched).
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/level_match_bypass_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"   # require_capture / capture_diagnosis
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
IDENTITY="$BUILD/identity_plugin_artefacts/VST3/Identity.vst3"
Q=960000
INSERT_GAIN=0.5

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }
[ -d "$IDENTITY" ] || { echo "build identity_plugin_VST3 first"; exit 2; }

TMP="$(mktemp -d)"
python3 - "$TMP/s.wav" <<'PY'
import sys, wave, struct, math
sr = 44100
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(0.6*32767*math.sin(2*math.pi*440*i/sr)))
                       for i in range(sr*20)))
w.close()
PY
cat > "$TMP/lm.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "lm" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [ { "id": 1, "name": "s", "length": $((64*Q)), "kind": "audio",
    "audio": { "source_path": "$TMP/s.wav", "source_start_frame": 0, "gain_db": 0.0,
               "fade_in": 0, "fade_out": 0 } } ],
  "tracks": [
    { "track_id": 0, "name": "A",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "placements": [ { "clip_id": 1, "at": 0, "length": $((64*Q)),
                        "notes": [], "chords": [], "mutes": [] } ] },
    { "track_id": 4294901760, "name": "Master", "is_master": true,
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [ { "device_id": 0, "kind": "vst_effect", "capability_mask": 4,
        "patcher_node_id": 4294967295, "host_slot_index": 4294967294, "bypass": false,
        "vst_ref": { "vendor": "daw", "name": "Identity", "path": "$IDENTITY", "uid16": "" } } ],
      "mod_links": [], "placements": [] } ] }
EOF

run() {  # run <extra-env> <name>
  local shm="/lmchk_${2}_$$"
  ( cd "$BUILD" && exec env DAW_IDENTITY_PASSTHRU=1 DAW_FAKE_INSERT_GAIN=$INSERT_GAIN $1 \
      DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" DAW_CAPTURE_WAV="$TMP/$2.wav" \
      DAW_CAPTURE_SECONDS=14 ./daw_engine --run-seconds 18 >"$TMP/$2.log" 2>&1 ) &
  local eng=$!
  sleep 2.5
  DAW_UI_SHM_NAME="$shm" "$CLI" do load lm --force >/dev/null 2>&1 || true
  sleep 1.5
  DAW_UI_SHM_NAME="$shm" "$CLI" do play --force >/dev/null 2>&1 || true
  sleep 6   # run ACTIVE long enough for the insert's gain to be measured
  DAW_UI_SHM_NAME="$shm" "$CLI" do set-bypass --track master --device 0 --bypass 1 --force \
    >/dev/null 2>&1 || true
  wait "$eng" 2>/dev/null || true
  sleep 1
}
run "" matched
run "DAW_NO_LEVEL_MATCH=1" raw

# PRECONDITION, asserted rather than assumed: the master FX path must actually have
# engaged in BOTH runs. If it never engaged the callback just passes the raw sum through
# for the whole capture, both windows read identical, and the ratio looks like a clean
# 1.00x — a passing-looking number from a run that tested nothing.
for r in matched raw; do
  if ! grep -q "Master FX: engaged" "$TMP/$r.log"; then
    echo "  FAIL (setup): master FX never engaged in the '$r' run — nothing was tested"
    rm -rf "$TMP"; exit 1
  fi
done

require_capture "$TMP/matched.wav" "$TMP/matched.log"
require_capture "$TMP/raw.wav" "$TMP/raw.log"
python3 - "$TMP/matched.wav" "$TMP/raw.wav" "$INSERT_GAIN" <<'PYX'
import sys, wave, numpy as np
# TIMING-FREE. Earlier versions picked wall-clock windows and kept mis-aligning with the
# real transitions (transport start, host ready, the bypass toggle all drift run to run),
# and a window straddling one averages two levels into a meaningless ratio. So never ask
# WHEN anything happened: slice the whole capture and ask how many distinct LEVELS it has.
#   raw     -> two plateaus (processed, then unprocessed) -> spread ~ 1/gain
#   matched -> one plateau  (bypass scaled to match)      -> spread ~ 1.0
def levels(path):
    w = wave.open(path, 'rb'); sr = w.getframerate(); ch = w.getnchannels()
    d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)/32768.0
    if ch > 1: d = d.reshape(-1, ch).mean(axis=1)
    win = int(0.25*sr); out = []
    for k in range(0, len(d)-win, win):
        r = float(np.sqrt((d[k:k+win]**2).mean()))
        if r > 1e-3:
            out.append(r)
    return np.array(out)
gain = float(sys.argv[3]); ok = True; spread = {}
for path, label in ((sys.argv[1], "matched"), (sys.argv[2], "raw")):
    lv = levels(path)
    if lv.size < 8:
        print("  FAIL (setup): '%s' captured almost no audio" % label); ok = False; continue
    lo, hi = float(np.percentile(lv, 10)), float(np.percentile(lv, 90))
    spread[label] = hi/lo if lo > 1e-6 else 0.0
    print("  %-8s lo=%.5f hi=%.5f  spread=%.2fx" % (label, lo, hi, spread[label]))
if ok:
    exp = 1.0/gain
    if abs(spread["raw"] - exp) > 0.25*exp:
        print("  FAIL: raw spread %.2fx, expected ~%.2fx — the run never toggled or the "
              "fixture gain did not apply, so the check is invalid" % (spread["raw"], exp))
        ok = False
    if spread["matched"] > 1.25:
        print("  FAIL: level-matched bypass still steps %.2fx" % spread["matched"])
        ok = False
    if ok:
        print("  raw steps %.2fx; level-matched stays %.2fx — A/B compares tone, not loudness"
              % (spread["raw"], spread["matched"]))
raise SystemExit(0 if ok else 1)
PYX
rc=$?
rm -rf "$TMP"
[ "$rc" = "0" ] && echo "level_match_bypass_check: PASS" \
                || { echo "level_match_bypass_check: FAIL"; exit 1; }
