#!/usr/bin/env bash
# End-to-end proof of Plugin Delay Compensation (Movement 4). Two instrument tracks
# play the SAME note at the SAME tick through the fake identity plugin: track 0 with
# zero latency (panned hard LEFT), track 1 with 512 samples of reported+applied
# latency (panned hard RIGHT). Because the pan isolates each track to one master
# channel, the L pulse is track 0 and the R pulse is track 1, so their sample offset
# is directly measurable in the captured master.
#
#   - PDC ON  (default): the engine delays the dry track by 512 to match the latent
#     one -> L and R land aligned (offset ~0).
#   - PDC OFF (DAW_DISABLE_PDC=1): the latent track's output still trails by 512, the
#     dry one is not delayed -> R lands ~512 samples after L.
#
# Alignment appearing ONLY when compensation runs is the proof. Needs a real audio
# device (non-test mode) and the C++ + daw-cli targets built.
#
#   tools/pdc_alignment_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000                 # nanoticks per quarter
NOTE_TICK=$((2 * Q))     # note at 1.0s @120bpm — clear of transport warm-up
CLIP_LEN=$((4 * Q))
LATENCY=512              # samples of fake plugin latency on the latent track
TMP="$(mktemp -d)"
SHM="/pdc_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

# --- The two-track project. Both tracks are fake-identity instruments; the latency
# is carried in the device name ("latency:N"), parsed host-side. host_slot_index is
# kHostSlotIndexDirect (0xFFFFFFFE) so the device loads the engine's default Identity
# binary by path instead of resolving through the plugin cache.
python3 - "$TMP/pdc.uniproj.json" "$Q" "$NOTE_TICK" "$CLIP_LEN" "$LATENCY" <<'PY'
import sys, json
out, Q, note_tick, clip_len, lat = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
DIRECT = 4294967294  # kHostSlotIndexDirect

def route(kind="none"):
    return {"kind": kind, "track_id": 0, "input_id": 0}

def routing():
    return {"midi_in": route(), "midi_out": route(), "audio_in": route(),
            "audio_out": route("master"), "pre_fader_send": True}

def device(name):
    return {"device_id": 0, "kind": "vst_instrument", "capability_mask": 5,
            "patcher_node_id": 0, "host_slot_index": DIRECT, "bypass": False,
            "vst_ref": {"vendor": "", "name": name, "path": "", "uid16": ""}}

def note(nid):
    return {"nanotick": note_tick, "duration": Q, "pitch": 60, "velocity": 100,
            "column": 0, "note_id": nid}

def clip(cid, nid):
    return {"id": cid, "name": f"n{cid}", "length": clip_len, "lines_per_beat": 4,
            "time_sig_numerator": 4, "time_sig_denominator": 4, "kind": "symbolic",
            "notes": [note(nid)], "chords": []}

def placement(cid):
    return {"clip_id": cid, "at": 0, "length": clip_len, "notes": [], "chords": [], "mutes": []}

def track(tid, name, pan, dev_name, cid):
    return {"track_id": tid, "name": name, "harmony_quantize": False, "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": pan, "mute": False, "solo": False},
            "routing": routing(), "device_chain": [device(dev_name)], "mod_links": [],
            "placements": [placement(cid)]}

doc = {
    "schema_version": 4,
    "meta": {"name": "pdc", "created_utc": 0, "modified_utc": 0},
    "nanoticks_per_quarter": Q,
    "tempo_map": [{"nanotick": 0, "bpm": 120.0}],
    "harmony_timeline": [],
    "clips": [clip(1, 1), clip(2, 2)],
    "tracks": [
        track(0, "Dry",   -1.0, "latency:0",          1),
        track(1, "Latent", 1.0, f"latency:{lat}",     2),
    ],
}
json.dump(doc, open(out, "w"), indent=2)
print("wrote", out)
PY

run_capture() {  # $1=label  $2=take.wav  $3=extra engine env ("" or "DAW_DISABLE_PDC=1")
  local label="$1" take="$2" extra="$3"
  local log="$TMP/engine_$label.log"
  # RENDERED OFFLINE. No sound card is involved in the answer this check asks, and the render
  # pump never skips a block or primes with silence, so a missing signal is a missing signal
  # rather than an underrun that may not repeat. The realtime pull path is pinned by
  # offline_render_check (a render against a device capture of the same fixture) and by the
  # checks that deliberately stay on hardware: audio_stability, sidechain, master_fx, panic,
  # preview_note, level_match_bypass.
  ( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" \
      DAW_PROJECT_DIR="$TMP" $extra \
      ./daw_engine --project pdc --render "$(basename " --sample-rate 44100$take" .wav)" --run-seconds 6 \
      >"$log" 2>&1 ) \
    || { echo "  FAIL: the '$label' render exited non-zero — see $log"; exit 1; }
  grep -o 'pdc.chain_latency[^)]*samples[^ )]*[0-9]*' "$log" | head -4 || true
}

echo "=== PDC ON ==="
run_capture on "$TMP/on.wav" ""
echo "=== PDC OFF ==="
run_capture off "$TMP/off.wav" "DAW_DISABLE_PDC=1"

echo "--- analysis ---"
python3 - "$TMP/on.wav" "$TMP/off.wav" "$LATENCY" <<'PY'
import sys, wave, struct
def onsets(path, thresh=0.25):
    w = wave.open(path, 'rb'); ch = w.getnchannels(); n = w.getnframes()
    raw = w.readframes(n); w.close()
    s = struct.unpack('<' + 'h' * (n * ch), raw)
    def first(c):
        for i in range(n):
            if abs(s[i * ch + c]) / 32768.0 > thresh:
                return i
        return -1
    return first(0), (first(1) if ch > 1 else -1), ch, n

on_l, on_r, ch, n = onsets(sys.argv[1])
off_l, off_r, *_ = onsets(sys.argv[2])
lat = int(sys.argv[3])
print(f"capture: channels={ch} frames={n}")
print(f"PDC ON : L onset={on_l}  R onset={on_r}  offset(R-L)={on_r-on_l}")
print(f"PDC OFF: L onset={off_l} R onset={off_r} offset(R-L)={off_r-off_l}")

ok = True
if on_l < 0 or on_r < 0 or off_l < 0 or off_r < 0:
    print("FAIL: a pulse was not detected in some channel (silent capture?)"); ok = False
else:
    on_off = abs(on_r - on_l)
    off_off = abs(off_r - off_l)
    # PDC ON: the two tracks must align within a small tolerance.
    if on_off > 16:
        print(f"FAIL: PDC ON tracks not aligned (offset {on_off} > 16)"); ok = False
    # PDC OFF: the latent track must trail by ~= its plugin latency (proves the
    # latency is real and that ON did the compensating, not that there was none).
    if not (lat * 0.75 <= off_off <= lat * 1.25):
        print(f"FAIL: PDC OFF offset {off_off} not ~= plugin latency {lat}"); ok = False

print("PASS: PDC aligns latent + dry tracks; disabling it reveals the raw offset" if ok else "RESULT: FAIL")
sys.exit(0 if ok else 1)
PY
rc=$?
rm -rf "$TMP"
[ $rc -eq 0 ] && echo "pdc_alignment_check: PASS" || { echo "pdc_alignment_check: FAIL"; exit 1; }
