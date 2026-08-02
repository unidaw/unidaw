#!/usr/bin/env bash
# Checks that the INSTRUMENT's per-insert meter slot is written (v24 metering).
#
# The host wrote hostDeviceMeters only when it had measured an INPUT level, and an
# instrument has no audio input — so its slot was never written at all. A never-written
# slot is zero, and zero millibels on this scale is FULL SCALE: every instrument on
# every track drew a meter pegged at the top with the transport stopped, which reads as
# clipping rather than as a bug. Reported by the frontend against v24, who declined to
# ship the meter cards until it was fixed.
#
# The assertion is deliberately about the value, not just presence: 0 is precisely the
# wrong number, and "the slot exists" would have passed before the fix too.
#
# The instrument is driven with DAW_FAKE_TONE_HZ so it emits a CONTINUOUS tone. Without
# it the fixture only pulses for 10 samples per note-on, so out_* read silent in ~91% of
# blocks whether the output metering works or not — and the first version of this check
# asserted only "all four read SILENT", which stayed green with the output-loop half of
# the fix reverted. It measures both halves now: in_* SILENT because an instrument has
# no audio input, out_* NOT silent because it is generating.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/instrument_meter_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
IDENTITY="$BUILD/identity_plugin_artefacts/VST3/Identity.vst3"
Q=960000
SILENT=-32768   # kUiMeterSilent

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }
[ -d "$IDENTITY" ] || { echo "build identity_plugin_VST3 first"; exit 2; }

TMP="$(mktemp -d)"
SHM="/imchk_$$"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/im.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "im" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [],
  "tracks": [
    { "track_id": 0, "name": "Inst",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [
        { "device_id": 3, "kind": "vst_instrument", "capability_mask": 5,
          "patcher_node_id": 4294967295, "host_slot_index": 4294967294, "bypass": false,
          "vst_ref": { "vendor": "daw", "name": "Identity", "path": "$IDENTITY", "uid16": "" } } ],
      "mod_links": [], "placements": [] } ] }
EOF

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    DAW_FAKE_TONE_HZ=440 \
    ./daw_engine --run-seconds 16 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load im >/dev/null 2>&1 || true
sleep 2
cli do play >/dev/null 2>&1 || true
sleep 2

METERS="$(cli get meters 2>/dev/null || true)"
echo "$METERS" | sed -n 's/^ *//p' | head -5

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

fail() { echo "  FAIL: $*"; exit 1; }

# PRECONDITION: the instrument's device must appear at all. Without this the value
# assertions below match nothing and pass vacuously.
echo "$METERS" | grep -q '"device": 3' || \
  fail "the instrument (device 3) has no meter entry — the host never wrote its slot,
        which is the bug this checks for, or the fixture failed to load"

LINE="$(echo "$METERS" | tr '{' '\n' | grep '"device": 3' | head -1)"
# BSD sed has no \? operator, so match the sign as a character class.
get() { echo "$LINE" | sed -n "s/.*\"$1\": \([-0-9][0-9]*\).*/\1/p"; }
IN_PEAK="$(get in_peak_mb)"; OUT_PEAK="$(get out_peak_mb)"
IN_RMS="$(get in_rms_mb)";   OUT_RMS="$(get out_rms_mb)"
echo "  instrument meters: in_peak=$IN_PEAK out_peak=$OUT_PEAK in_rms=$IN_RMS out_rms=$OUT_RMS"

for pair in "in_peak $IN_PEAK" "out_peak $OUT_PEAK" "in_rms $IN_RMS" "out_rms $OUT_RMS"; do
  name="${pair%% *}"; value="${pair##* }"
  [ -n "$value" ] || fail "$name was not present in the meter entry"
  [ "$value" != "0" ] || \
    fail "$name reads 0 mB, which on this scale is FULL SCALE, not silence — the
        instrument's slot is still never written"
done

# An instrument has no audio input, so the in_* pair must say SILENT rather than
# inventing a level.
[ "$IN_PEAK" = "$SILENT" ] && [ "$IN_RMS" = "$SILENT" ] || \
  fail "an instrument has no audio input, so in_peak/in_rms must be kUiMeterSilent
        ($SILENT); got $IN_PEAK / $IN_RMS"

# And its OUTPUT must be measured. This is the half the first version of this check
# could not see: with the output loop bounded by the INPUT channel count, an instrument
# measures zero channels and reads silent, which was indistinguishable from "the
# generator made no sound".
[ "$OUT_PEAK" != "$SILENT" ] && [ "$OUT_RMS" != "$SILENT" ] || \
  fail "the instrument is generating a 440 Hz tone, but its out_peak/out_rms read
        kUiMeterSilent — the output is not being metered (the loop is probably still
        bounded by the input channel count, which is 0 for an instrument)"
# A tone at 0.25 amplitude is about -1200 mB peak. Assert a sane range rather than an
# exact number, but tight enough that a stray 0 or a full-scale reading fails.
[ "$OUT_PEAK" -lt 0 ] && [ "$OUT_PEAK" -gt -3000 ] || \
  fail "out_peak is $OUT_PEAK mB; a 0.25-amplitude tone should read about -1200"

echo "instrument_meter_check: PASS — the instrument's meter is written, and reads"
echo "  silence rather than full scale"
