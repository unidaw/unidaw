#!/usr/bin/env bash
# THE BANK'S GATE DEFAULT SEEDS A NEW SLOT AND LEAVES THE OLD ONES ALONE.
#
# `gate` decides whether a note-off cuts a sampled voice. It defaulted to 0 — one-shot, ignores
# note-off — for every slot `sampler-load` and `sampler-slice` create, and NOBODY CHOSE THAT:
# neither path sets `gate` at all, so the slot kept `SamplerSlot`'s `uint8_t gate = 0` and zero
# happens to mean one-shot. Right for drums by accident, wrong for anything you want to cut, and
# after chopping a break into 64 slices you were setting the same field 64 times.
#
# Owner: "could that be a setting per bank? 'ignore note-offs'". So `SamplerState::defaultGate`,
# reachable by opcode 88.
#
# IT SEEDS, IT DOES NOT OVERRIDE, and that distinction is the whole check. A device flag that
# overrode slots at playback would be two facts about one thing for the voice to arbitrate on
# every note — the shape that had the kit read-back disagreeing with the model and the patcher
# pool disagreeing with the device graphs. Here the default is consumed at MINT and the slot's own
# `gate` is the authority from that moment.
#
# FOUR PROPERTIES, and the second is the one a read-back could not check:
#   SEEDED     a slot minted AFTER setting the default comes out gated
#   UNTOUCHED  the slot that already existed keeps gate 0 — this is what distinguishes a seed
#              from an override, and no amount of reading the default back can tell them apart
#   AUDIBLE    a short note on the SEEDED slot stops early, because gate is the field that decides
#              whether note-off cuts the voice. A default that persists and never reaches the
#              voice is the same defect one layer along, which is what filterType turned out to be
#   CONTRAST   the same short note on the UNTOUCHED slot runs on — the control, without which
#              "stops early" could just mean the sample ran out
#
#   tools/sampler_default_gate_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
# stop_engine, not a bare kill: it SIGTERMs, waits 10s, ESCALATES to SIGKILL and SAYS SO.
# A hung engine that ignores SIGTERM is left running by a bare kill, and ctest then waits
# about 1000s for it after timing the check out — measured across 18 runs, perfectly
# correlated with the timeout count. The escalation notice is also the diagnostic: "engine
# N ignored SIGTERM for 10s" names the hang instead of leaving it to be inferred.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
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
fail() { echo "  FAIL: $*"; exit 1; }

# A THREE-SECOND STEADY TONE. Long, because the whole measurement is "does a half-second note cut
# it": a short sample would stop on its own and the check would be reading the sample's length.
python3 - "$TMP/tone.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(13000 * math.sin(2 * math.pi * 440.0 * i / sr)))
                       for i in range(sr * 3)))
w.close()
PY

# ONE EXISTING SLOT, gate 0, on key 60. It is the "untouched" case and it must be there BEFORE the
# default is set — a fixture whose only slot is minted afterwards cannot tell a seed from an
# override, because there would be nothing the override could have failed to touch.
python3 - "$TMP/g.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "old", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 60, "key_high": 60, "root_key": 60,
        "pitch_track_milli": 0, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "tone.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
# TWO SHORT NOTES, FOUR SECONDS APART — and the gap is the fixture's whole correctness.
# The first slot is a ONE-SHOT on a three-second sample, so it rings until 3.0 s no matter how
# short its note is. A first attempt put the second note one bar (2.0 s) later and measured
# 11259 where one slot gives 5630: the window for "is the gated slot still sounding" contained
# the one-shot's tail, and the check failed on the fixture rather than on the engine. Two bars
# puts the second note at 4.0 s, a clear second after the first has run out.
notes = [{"nanotick": 0, "duration": Q // 2, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": 1},
         {"nanotick": BAR * 2, "duration": Q // 2, "pitch": 72, "velocity": 110,
          "column": 1, "note_id": 2}]
tr = {"track_id": 0, "name": "G", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 4,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "g"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR * 4, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY

SHM="/defgate_$$"
( cd "$BUILD" && exec env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" \
    ./daw_engine --project g --run-seconds 25 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 40
gcli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }

# ---- Set the bank's default, THEN mint a slot with it.
KV_BEFORE="$(gcli get sampler-kit --track 0 --device 1 2>/dev/null \
             | python3 -c "import json,sys; print(json.load(sys.stdin).get('kit_version'))")"
gcli do sampler-device --track 0 --device 1 --field default-gate --value 1 >/dev/null 2>&1
sleep 0.6
grep -q '"event":"sampler.device_set"' "$TMP/eng.log" 2>/dev/null || \
  fail "opcode 88 never reached the engine — no sampler.device_set in $TMP/eng.log. The command
        was not dispatched, so everything below would be measuring the fixture's own gate values"

# READ IT BACK, because a toggle whose state cannot be read is one a UI has to invent — and a
# bank's default cannot be inferred from its slots, since a bank legitimately mixes one-shot and
# gated ones.
kitfield() {  # kitfield <key>
  gcli get sampler-kit --track 0 --device 1 2>/dev/null \
    | python3 -c "import json,sys; print(json.load(sys.stdin).get('$1'))"
}
DG="$(kitfield default_gate)"
[ "$DG" = "1" ] || fail "the kit read-back reports default_gate '$DG' after setting it to 1"
echo "  read-back: default_gate $DG, kit_version $KV_BEFORE -> $(kitfield kit_version)"

# ---- THE VERSION MUST MOVE, and this is the web-UI agent's requirement rather than my idea.
# Their cache holds a kit answer until kit_version changes, so a device field that becomes part of
# the answer without moving the version updates NOTHING on screen until something else happens to
# change the kit — the card shows the old value while the engine has the new one. That is the same
# shape as the voice count they could not read live: 72 polls produced four publications because
# the version had not moved.
#
# It passes because refreshSamplerForTrack is the one funnel that bumps it, and the handler calls
# it — but that call was ADDED after a first version deliberately skipped it, so this is the
# assertion that stops the next optimisation from quietly reintroducing the problem.
KV_AFTER="$(kitfield kit_version)"
python3 -c "
raise SystemExit(0 if int('$KV_AFTER') > int('$KV_BEFORE') else 1)" || \
  fail "setting a device field did not move kit_version ($KV_BEFORE -> $KV_AFTER). A client that
        caches the kit answer until the version changes will never see this field update — it
        will show the old value while the engine holds the new one"

gcli do sampler-load --track 0 --device 1 --file tone.wav --root 72 --fixed >/dev/null 2>&1
sleep 1.2
gcli do save out >/dev/null 2>&1
sleep 1.5
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -f "$TMP/out.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"

# ---- SEEDED + UNTOUCHED, from the save.
python3 - "$TMP/out.uniproj.json" <<'PYC' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
slots = []
for t in d["tracks"]:
    for dev in t["device_chain"]:
        slots = dev.get("sampler", {}).get("slots", [])
gates = {s["id"]: s.get("gate") for s in slots}
print("  slot gates after the load: %r" % gates)
if len(gates) < 2:
    print("  FAIL: the load did not mint a second slot, so there is nothing to have seeded")
    raise SystemExit(1)
if gates.get(1) != 0:
    print("  FAIL: slot 1 existed BEFORE the default was set and its gate is now %r. The default"
          " must SEED a new slot, not reach back and rewrite the ones already there — that is the"
          " difference between a default and an override, and an override is two facts about one"
          " thing for the voice to arbitrate on every note." % gates.get(1))
    raise SystemExit(1)
if gates.get(2) != 1:
    print("  FAIL: slot 2 was minted AFTER default-gate was set to 1 and its gate is %r. The"
          " default is stored and not stamped, so sampler-load is still handing out whatever"
          " SamplerSlot's initialiser says." % gates.get(2))
    raise SystemExit(1)
print("  seeded: the new slot is gated, the existing one is untouched")
PYC

# ---- AUDIBLE + CONTRAST. gate is the field that decides whether note-off cuts the voice, so a
# default that saves and never reaches the voice must fail here.
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/defgate_r_$$" \
    ./daw_engine --project out --render take --run-seconds 9 --block-size 256 \
    >"$TMP/render.log" 2>&1 ) || fail "the render exited non-zero — see $TMP/render.log"

peak() {  # peak <fromSec> <toSec>
  python3 - "$TMP/take.wav" "$1" "$2" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(sr * float(sys.argv[2])), int(sr * float(sys.argv[3]))
seg = [abs(s[i * ch]) for i in range(max(a, 0), min(b, n))]
print(max(seg) if seg else 0)
PY
}

# Slot 1's note is at tick 0 and lasts 0.25 s, but it is a ONE-SHOT on a three-second sample, so
# it rings to 3.0 s. Its "late" window is inside that ring, well past its own note.
OLD_EARLY="$(peak 0.05 0.20)"
OLD_LATE="$(peak 2.00 2.80)"
# Slot 2's note is two bars in — 4.0 s — a clear second after the one-shot has run out, so its
# window contains only itself.
NEW_EARLY="$(peak 4.05 4.20)"
NEW_LATE="$(peak 4.60 5.50)"
echo "  slot 1 (one-shot): $OLD_EARLY early, $OLD_LATE late"
echo "  slot 2 (seeded):   $NEW_EARLY early, $NEW_LATE late"

[ "${OLD_EARLY:-0}" -gt 2000 ] && [ "${NEW_EARLY:-0}" -gt 2000 ] || \
  fail "one of the two notes never sounded at all ($OLD_EARLY / $NEW_EARLY), so the comparison
        below would be about a broken fixture rather than about gate"

# CONTRAST first: the untouched one-shot must still be ringing well past its note.
[ "${OLD_LATE:-0}" -gt 2000 ] || \
  fail "the UNTOUCHED slot (gate 0, one-shot) went quiet by 0.6 s (peak ${OLD_LATE:-0}) although
        its sample is three seconds long. Something else is cutting it, so 'the seeded slot stops
        early' below could not be attributed to gate"

# AUDIBLE: the seeded slot must be cut by its note-off.
[ "${NEW_LATE:-0}" -lt 500 ] || \
  fail "the SEEDED slot is still sounding at 0.6 s past its note (peak ${NEW_LATE:-0}) where the
        one-shot gives $OLD_LATE. Its gate saved as 1 and the voice is not honouring it — the
        default reached the file and not the sound, which is the same defect one layer along"

echo "sampler_default_gate_check: PASS — the bank's default seeds a new slot, leaves the old ones"
echo "                            alone, and the seeded slot is cut by its note-off"
