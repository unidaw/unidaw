#!/usr/bin/env bash
# End-to-end proof of sidechain routing (Movement 4). Track 0 plays a note through the
# fake identity plugin (a pulse), panned hard LEFT, and is the sidechain SOURCE for
# track 1. Track 1 runs the fake with its sidechain input enabled and has NO notes of
# its own — so its only possible output is whatever arrives on its sidechain bus, panned
# hard RIGHT. If the key signal is routed and reaches the plugin, R carries track 0's
# pulse; if not, R is silent.
#
#   - bound   (track 1 sidechain = track 0): R is non-silent and tracks L.
#   - unbound (no sidechain route):          R is silent.
#
# R being audible ONLY when the route exists is the proof. Needs a real audio device
# (non-test mode) and the C++ + daw-cli targets built.
#
#   tools/sidechain_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
NOTE_TICK=$((2 * Q))
CLIP_LEN=$((4 * Q))
TMP="$(mktemp -d)"
SHM="/sc_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

gen_project() {  # $1=out  $2=bound(1/0)
  python3 - "$1" "$2" "$Q" "$NOTE_TICK" "$CLIP_LEN" <<'PY'
import sys, json
out, bound, Q, note_tick, clip_len = sys.argv[1], sys.argv[2]=="1", int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
DIRECT = 4294967294

def route(kind="none", tid=0):
    return {"kind": kind, "track_id": tid, "input_id": 0}

def routing(sidechain=None):
    r = {"midi_in": route(), "midi_out": route(), "audio_in": route(),
         "audio_out": route("master"), "pre_fader_send": True}
    if sidechain is not None:
        r["sidechain"] = sidechain
    return r

def device():
    return {"device_id": 0, "kind": "vst_instrument", "capability_mask": 5,
            "patcher_node_id": 0, "host_slot_index": DIRECT, "bypass": False,
            "vst_ref": {"vendor": "", "name": "identity", "path": "", "uid16": ""}}

def note(nid):
    return {"nanotick": note_tick, "duration": Q, "pitch": 60, "velocity": 100,
            "column": 0, "note_id": nid}

def clip(cid, notes):
    return {"id": cid, "name": f"n{cid}", "length": clip_len, "lines_per_beat": 4,
            "time_sig_numerator": 4, "time_sig_denominator": 4, "kind": "symbolic",
            "notes": notes, "chords": []}

def placement(cid):
    return {"clip_id": cid, "at": 0, "length": clip_len, "notes": [], "chords": [], "mutes": []}

# Track 0: the key source — plays a pulse, panned L. Track 1: sidechained, no notes, panned R.
src_sc = route("track", 0) if bound else None
tracks = [
  {"track_id": 0, "name": "Key", "harmony_quantize": False, "lines_per_beat": 4,
   "mixer": {"gain_db": 0.0, "pan": -1.0, "mute": False, "solo": False},
   "routing": routing(), "device_chain": [device()], "mod_links": [],
   "placements": [placement(1)]},
  {"track_id": 1, "name": "Ducked", "harmony_quantize": False, "lines_per_beat": 4,
   "mixer": {"gain_db": 0.0, "pan": 1.0, "mute": False, "solo": False},
   "routing": routing(src_sc), "device_chain": [device()], "mod_links": [],
   "placements": [placement(2)]},
]
doc = {
    "schema_version": 4,
    "meta": {"name": "sc", "created_utc": 0, "modified_utc": 0},
    "nanoticks_per_quarter": Q,
    "tempo_map": [{"nanotick": 0, "bpm": 120.0}],
    "harmony_timeline": [],
    "clips": [clip(1, [note(1)]), clip(2, [])],  # track 1's clip has NO notes
    "tracks": tracks,
}
json.dump(doc, open(out, "w"), indent=2)
PY
}

run_capture() {  # $1=name  $2=take
  local name="$1" take="$2"
  local log="$TMP/engine_$name.log"
  # A SEGMENT PER RUN, and readiness read from the log rather than slept for.
  #
  # This failed once inside a full-suite run — "bound R is silent, the sidechain key never
  # reached the plugin" — and passed 3/3 immediately afterwards on the same binary. Two
  # causes, both in the harness: a 6-second engine with `sleep 2` then `sleep 1` left barely
  # 3 seconds of playback for a host that has to come up and negotiate an aux INPUT bus
  # before the key can arrive, and both takes shared one shm name, so the second one could
  # find the first take's segment still mapped and send `do load` into a ring nobody was
  # reading (the bug that made multiout_check test its first pitch twice).
  # The engine must reach its own exit: the capture ring is flushed to disk on clean
  # shutdown, so killing it early leaves no WAV at all. 16 seconds is setup plus generous
  # playback, not a wait for something.
  local shm="${SHM}_$name"
  ( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$shm" \
      DAW_PROJECT_DIR="$TMP" DAW_CAPTURE_WAV="$take" DAW_CAPTURE_SECONDS=20 \
      ./daw_engine --run-seconds 16 >"$log" 2>&1 ) &
  local engine=$!
  # BOTH WAITS ASSERT, and both watch the pid. This engine lives 16 seconds and these loops used
  # to be willing to wait 30 and 20 — so a slow start under a parallel ctest meant waiting out a
  # corpse and then carrying on to `do play` against a process that was already gone, which
  # surfaces as an empty capture rather than as "it never started". Task #106.
  wait_for_boot "$log" "$engine" 120 "starting threads"
  DAW_UI_SHM_NAME="$shm" "$CLI" do load "$name" --force >/dev/null 2>&1 || true
  # This engine starts with NO project, so the load below is the FIRST one — but count it rather
  # than grep for it, so the wait stays correct if a boot project is ever added.
  wait_for_loads "$log" "$engine" 1 80
  sleep 1.5   # let the host finish bringing up its buses before the key is expected
  DAW_UI_SHM_NAME="$shm" "$CLI" do play --force >/dev/null 2>&1 || true
  wait "$engine"
  grep -q '"event":"audio.capture_written"' "$log" || \
    echo "  (warning: $name produced no capture — the analysis below has nothing to read)" >&2
}

gen_project "$TMP/bound.uniproj.json" 1
gen_project "$TMP/unbound.uniproj.json" 0
echo "=== bound (track 1 sidechain <- track 0) ==="
run_capture bound "$TMP/bound.wav"
echo "=== unbound (no sidechain route) ==="
run_capture unbound "$TMP/unbound.wav"

echo "--- analysis ---"
python3 - "$TMP/bound.wav" "$TMP/unbound.wav" <<'PY'
import sys, wave, struct
def stats(path):
    w = wave.open(path, 'rb'); ch = w.getnchannels(); n = w.getnframes()
    s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
    def peak(c): return max((abs(s[i*ch+c]) for i in range(n)), default=0) / 32768.0
    def onset(c, th=0.2):
        for i in range(n):
            if abs(s[i*ch+c]) / 32768.0 > th:
                return i
        return -1
    return ch, n, peak(0), peak(1), onset(0), onset(1)

bch, bn, bL, bR, boL, boR = stats(sys.argv[1])
uch, un, uL, uR, uoL, uoR = stats(sys.argv[2])
print(f"bound   : L peak={bL:.3f}@{boL}  R peak={bR:.3f}@{boR}")
print(f"unbound : L peak={uL:.3f}@{uoL}  R peak={uR:.3f}@{uoR}")

ok = True
# Bound: L (the key source) must sound, and R (sidechain passthrough) must too.
if bL < 0.3:
    print("FAIL: bound L (key source) is silent — the source never played"); ok = False
if bR < 0.3:
    print("FAIL: bound R is silent — the sidechain key never reached the plugin"); ok = False
# R must follow L, not lead it (the key is pulled a couple blocks late).
if ok and boR >= 0 and boL >= 0 and boR < boL - 64:
    print(f"FAIL: bound R onset {boR} precedes L {boL} — not a keyed passthrough"); ok = False
# Unbound: R must be silent (track 1 has no notes and no key).
if uR > 0.05:
    print(f"FAIL: unbound R peak {uR:.3f} not silent — output without a sidechain route"); ok = False

print("PASS: the sidechain key reaches the plugin only when routed" if ok else "RESULT: FAIL")
sys.exit(0 if ok else 1)
PY
rc=$?
rm -rf "$TMP"
[ $rc -eq 0 ] && echo "sidechain_check: PASS" || { echo "sidechain_check: FAIL"; exit 1; }
