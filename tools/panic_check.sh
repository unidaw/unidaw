#!/usr/bin/env bash
# Checks PANIC (UiCommandType 52): all sound off, and it STAYS off.
#
# Stop halts, rewinds and releases held keyjazz notes — correct, but not a panic: it
# cannot reach a plugin's own ringing voices, a sequencer note whose note-off has not been
# reached, or a generator mid-phrase. Panic sends CC120 (all-sound-off) AND CC123
# (all-notes-off) on every MIDI channel to every hosted plugin and drops the engine's note
# bookkeeping. CC120 is the one that matters: CC123 merely releases notes and lets a pad
# ring out, which is not what a person reaching for panic wants.
#
# The stimulus is maximal (six Zebra2 tracks, two of them generator-driven) played for a
# few seconds, so real sequencer notes AND a generator mid-phrase are sounding when panic
# lands. PASS requires audio before, silence right after, and silence that PERSISTS —
# sound coming back on its own is exactly what makes a panic button untrustworthy.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built, and
# Zebra2 (the maximal preset's instrument).
#   tools/panic_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
SHM="/panic_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }
[ -d "/Library/Audio/Plug-Ins/VST3/Zebra2.vst3" ] || { echo "SKIP: Zebra2 not installed"; exit 0; }

TMP="$(mktemp -d)"
( cd "$BUILD" && DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$ROOT/presets/projects" \
    DAW_CAPTURE_WAV="$TMP/p.wav" DAW_CAPTURE_SECONDS=14 \
    ./daw_engine --run-seconds 17 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
sleep 2.5
DAW_UI_SHM_NAME="$SHM" "$CLI" do load maximal --force >/dev/null 2>&1 || true
sleep 1
DAW_UI_SHM_NAME="$SHM" "$CLI" do play --force >/dev/null 2>&1 || true
sleep 5
DAW_UI_SHM_NAME="$SHM" "$CLI" do panic --force >/dev/null 2>&1 || true
wait "$ENG" 2>/dev/null || true

ok=1
grep -q "PANIC" "$TMP/engine.log" || { echo "  FAIL: the engine never saw the panic command"; ok=0; }
[ -s "$TMP/p.wav" ] || { echo "  FAIL (setup): no capture was written"; tail -3 "$TMP/engine.log" | sed 's/^/    /'; ok=0; }

if [ "$ok" = "1" ]; then
  python3 - "$TMP/p.wav" <<'PY' || ok=0
import sys, wave, numpy as np
w = wave.open(sys.argv[1], 'rb'); sr = w.getframerate(); ch = w.getnchannels()
d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)/32768.0
if ch > 1: d = d.reshape(-1, ch).mean(axis=1)
def rms(a, b):
    seg = d[int(a*sr):int(b*sr)]
    return float(np.sqrt((seg**2).mean())) if seg.size else 0.0
before, after, tail = rms(4, 7), rms(9, 11), rms(11, 13)
print(f"  rms before panic={before:.5f}  after={after:.5f}  tail={tail:.5f}")
ok = True
if before < 1e-4:
    print("  FAIL (setup): nothing was sounding before the panic — the check proves nothing")
    ok = False
if after > 1e-4:
    print("  FAIL: sound survived the panic")
    ok = False
if tail > 1e-4:
    print("  FAIL: sound CAME BACK after the panic")
    ok = False
if ok:
    print("  sound stopped on panic and stayed stopped")
raise SystemExit(0 if ok else 1)
PY
fi

rm -rf "$TMP"
[ "$ok" = "1" ] && echo "panic_check: PASS — panic cuts all sound and it stays cut" \
                || { echo "panic_check: FAIL"; exit 1; }
