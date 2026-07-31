#!/usr/bin/env bash
# A SOUND-ADDRESSED-ONLY TRACK PLAYS A DIFFERENT SLOT FOR THE SAME NOTE.
#
# Owner ruling, docs/SAMPLER_DESIGN.md section 8 Q2. By default a blank `sound` means "the keymap
# picks the slot from pitch" (R5). A track can be switched so pitch NEVER selects: the note's
# `sound` names the slot, pitch is varispeed, and a 64-slot kit stays fully chromatic rather than
# one slot per key. A blank `sound` under the flag plays the LOWEST slot id, because something has
# to sound or the keyboard goes silent — which is the very thing the ruling was weighing.
#
# THE DISCRIMINATOR IS STRUCTURAL, NOT A READ-BACK. Two slots with audibly different content and a
# keymap that sends the test pitch to the SECOND one:
#
#   slot 1   a 400 Hz tone, rootKey 60, mapped to keys 0..71
#   slot 2   a 1200 Hz tone, rootKey 72, mapped to keys 72..127
#
# One note, pitch 72, `sound` blank:
#
#   flag OFF   the keymap picks slot 2, played at its own root -> 1200 Hz
#   flag ON    pitch may not select, so the lowest slot id wins -> slot 1 at +12 -> 800 Hz
#
# So the assertion is which TONE comes out, which no amount of correct-looking state can fake. A
# check that read the flag back would pass on a flag that is stored and never consulted — the
# exact defect shape this suite has now found six times (filterType, loopMode, every patcher node
# config, the trim positions, ownerDeviceId, and the default envelope's own shape).
#
# THREE PHASES, because a field has three ways to be wrong:
#   PROJECT-OFF   the default still resolves through the keymap  (the control: without it, "on"
#                 producing 800 Hz could just mean the keymap was broken all along)
#   PROJECT-ON    set in the file, it changes the slot
#   COMMANDED     set by opcode 87 on a running engine, it changes the slot LIVE and it persists.
#                 Live capture, not a re-render, because a flag that saves and never reaches the
#                 voice is the same defect one layer along.
#
#   tools/sound_addressed_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# TWO TONES, both steady and both long enough to outlast the note, so what is measured is which
# SLOT is sounding rather than which one ran out of sample first.
python3 - "$TMP/lo.wav" 400 <<'PY'
import sys, wave, struct, math
sr = 48000
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(13000 * math.sin(2 * math.pi * float(sys.argv[2]) * i / sr)))
                       for i in range(sr * 3)))
w.close()
PY
python3 - "$TMP/hi.wav" 1200 <<'PY'
import sys, wave, struct, math
sr = 48000
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(13000 * math.sin(2 * math.pi * float(sys.argv[2]) * i / sr)))
                       for i in range(sr * 3)))
w.close()
PY

# project <name> <soundAddressedOnly 0|1>
project() {
  python3 - "$TMP/$1.uniproj.json" "$Q" "$2" <<'PY'
import json, sys
out, Q, addressed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def slot(sid, src, root, klow, khigh):
    return {"id": sid, "name": "s%d" % sid, "source_local_id": src, "slice_id": 0,
            "start_frame": 0, "end_frame": 0,
            "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0,
            "key_low": klow, "key_high": khigh, "root_key": root,
            # VARISPEED ON. Without it the lowest slot would play at its own pitch under the flag
            # and the two cases would differ for the wrong reason.
            "pitch_track_milli": 1000, "tune_cents": 0,
            "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
            "gate": 1, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
            "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
            "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 3, "next_source_id": 3, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 8, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "lo.wav", "content_key": 0},
                               {"local_id": 2, "path": "hi.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   # SLOT 1 IS THE LOWEST ID and is mapped AWAY from the test pitch, so "the
                   # keymap answered" and "the lowest slot answered" can never be the same slot.
                   "slots": [slot(1, 1, 60, 0, 71), slot(2, 2, 72, 72, 127)]}}
# ONE note at pitch 72, `sound` omitted (0). Shorter than the clip — a note whose duration equals
# its clip length renders silent (task #101) and would make every phase here read as a failure.
notes = [{"nanotick": 0, "duration": Q * 3, "pitch": 72, "velocity": 110,
          "column": 0, "note_id": 1}]
tr = {"track_id": 0, "name": "S", "harmony_quantize": False,
      "sound_addressed_only": bool(addressed),
      "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "a"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY
}

render() {  # render <project> <name>
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/sndaddr_${$}_$2" \
      ./daw_engine --project "$1" --render "$2" --run-seconds 4 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

# WHICH TONE IS IT? Energy at 800 Hz against energy at 1200 Hz, by direct correlation rather than
# a peak-picker — two candidates are known in advance, so there is nothing to search for and
# nothing to mis-detect. Prints "800", "1200" or "neither".
which_tone() {  # which_tone <name>
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
# A window well inside the note, past any attack and any capture lead-in.
a, b = int(sr * 0.30), int(sr * 0.90)
seg = [s[i * ch] for i in range(a, min(b, n))]
if not seg or max(abs(v) for v in seg) < 1500:
    print("silent"); raise SystemExit(0)
def energy(f):
    re = sum(v * math.cos(2 * math.pi * f * i / sr) for i, v in enumerate(seg))
    im = sum(v * math.sin(2 * math.pi * f * i / sr) for i, v in enumerate(seg))
    return math.hypot(re, im)
e800, e1200 = energy(800.0), energy(1200.0)
# A clear winner, not a nose. Anything closer than 3x means the take is not one of these tones.
if e800 > e1200 * 3:
    print("800")
elif e1200 > e800 * 3:
    print("1200")
else:
    print("neither(800=%.0f 1200=%.0f)" % (e800, e1200))
PY
}

# ---- PROJECT-OFF. The control. The keymap sends pitch 72 to slot 2, which is the 1200 Hz tone.
project off 0
render off off
OFF="$(which_tone off)"
echo "  flag off, project:      $OFF Hz"
[ "$OFF" = "1200" ] || \
  fail "with the flag OFF, pitch 72 should resolve through the keymap to slot 2 — the 1200 Hz
        tone — and the take is '$OFF'. The default resolution is broken, so nothing below can
        attribute a change to the flag"

# ---- PROJECT-ON. Same note, same fixture, one boolean.
project on 1
render on on
ON="$(which_tone on)"
echo "  flag on, project:       $ON Hz"
[ "$ON" = "800" ] || \
  fail "with sound_addressed_only ON, pitch 72 must NOT select a slot: the lowest slot id (1, the
        400 Hz tone at rootKey 60) plays at +12, which is 800 Hz. The take is '$ON'. If it is
        1200 the flag was read from the file and never reached the dispatch — the field is stored
        and not consulted, which is a defect this suite has found six times"

# ---- COMMANDED. Opcode 87 on a running engine: it must change the SOUND, and it must persist.
# The engine must exit on its own — the capture wav is written during clean shutdown, so killing
# it yields no file at all, which reads as silence and is not.
SHM="/sndaddr_cmd_$$"
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" \
    DAW_CAPTURE_WAV="$TMP/cmd.wav" DAW_CAPTURE_SECONDS=6 \
    ./daw_engine --project off --run-seconds 22 >"$TMP/cmd.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 40); do
  grep -q '"event":"project.load"' "$TMP/cmd.log" 2>/dev/null && break
  sleep 0.25
done
grep -q '"event":"project.load"' "$TMP/cmd.log" 2>/dev/null || \
  fail "the engine never loaded — see $TMP/cmd.log"
ccli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }

# ---- THE READ-BACK, BOTH WAYS. A toggle whose state cannot be read is one the interface has to
# invent, and for this flag inventing it means drawing the kit's mapping backwards. Asserted
# BEFORE and AFTER, because a bit that is always set reads exactly like a bit that is correct —
# the same blind spot as a one-device owner check.
read_addressed() {
  ccli get tracks 2>/dev/null > "$TMP/tracks.json"
  python3 - "$TMP/tracks.json" <<'PYR'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print("unreadable"); raise SystemExit(0)
t = d.get("tracks", [])
print(str(t[0].get("sound_addressed")).lower() if t else "notracks")
PYR
}
BEFORE="$(read_addressed)"
[ "$BEFORE" = "false" ] || \
  fail "before the command, the published sound_addressed bit reads '$BEFORE' and the project
        has the flag off. If it is already true the bit is stuck on and the assertion after the
        command would pass no matter what the command did"

ccli do sound-addressed --track 0 --on 1 >/dev/null 2>&1
sleep 0.8
grep -q '"event":"track.sound_addressed"' "$TMP/cmd.log" 2>/dev/null || \
  fail "opcode 87 never reached the engine — no track.sound_addressed event in $TMP/cmd.log.
        The command was not dispatched, so the audio below would be measuring the project's
        setting rather than the command's"
AFTER="$(read_addressed)"
[ "$AFTER" = "true" ] || \
  fail "after opcode 87 the published sound_addressed bit still reads '$AFTER'. The engine acted
        on the command — the audio below proves it — and did not publish it, so a UI reloading
        this project would draw the toggle off while the kit plays addressed"
echo "  read-back: false before the command, true after"

ccli do save cmdsaved >/dev/null 2>&1
sleep 1.0
ccli do play --force >/dev/null 2>&1 || true
wait "$ENG" 2>/dev/null; ENG=""

[ -f "$TMP/cmdsaved.uniproj.json" ] || fail "the engine did not save — see $TMP/cmd.log"
python3 - "$TMP/cmdsaved.uniproj.json" <<'PYC' || fail "sound-addressed did not persist. The
        command changed the running engine and the save did not carry it, so the setting is lost
        on reload — heard, and not saved"
import json, sys
d = json.load(open(sys.argv[1]))
for t in d["tracks"]:
    if t.get("sound_addressed_only") is True:
        raise SystemExit(0)
    print("  sound_addressed_only is %r, expected True" % t.get("sound_addressed_only"))
raise SystemExit(1)
PYC
echo "  commanded: persisted in the save"

CMD="$(which_tone cmd)"
echo "  flag on by command:     $CMD Hz"
[ "$CMD" = "800" ] || \
  fail "opcode 87 was received and persisted, and the LIVE audio is '$CMD' rather than 800 Hz.
        The command reached the model and not the RT snapshot the dispatch actually reads — two
        facts about one thing, and only the one nobody hears was updated"

echo "sound_addressed_check: PASS — the same note plays a different slot with the flag set, from"
echo "                       the project and from opcode 87, and the command persists"
