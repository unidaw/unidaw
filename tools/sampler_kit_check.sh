#!/usr/bin/env bash
# A KIT: EIGHT SOUNDS ON EIGHT KEYS, LAID DOWN IN ONE GESTURE, CHOKING AND RINGING CORRECTLY.
#
# The unit tests already pin choke, NNA and layering against float buffers (sampler_engine_tests).
# This is the same behaviour through the REAL engine — the scheduler, the host input plane, the
# offline render — because a rule that holds in a test harness and not in the program is a rule
# nobody has.
#
# EVERY SLOT PLAYS A DIFFERENT PITCH ON PURPOSE. Eight copies of one sound would let a kit that
# resolves every key to slot 1 pass perfectly: the output would be identical either way. Eight
# distinguishable tones mean "the wrong drum played" is visible as the wrong frequency, which is
# the difference between a check and a formality.
#
# FOUR PROPERTIES:
#   LAYS DOWN   one --files command puts N samples on N consecutive keys, fixed pitch
#   ADDRESSES   each key plays ITS OWN slot — asserted by frequency, not by "something sounded"
#   CHOKES      two slots in one voice group cut each other; a third, ungrouped, does not
#   RINGS       NNA=Continue lets the previous note keep sounding, which is the gesture a
#               truncate-on-entry model cannot express at all (the tracker survey's #1 item)
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/sampler_kit_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
# THE ENGINE MUST DIE WHEN THIS CHECK DOES, including when ctest KILLS the check on a timeout.
# This trap used to remove $TMP and leave the engine running: it was only stopped on the normal
# path and inside fail(). A timed-out check therefore orphaned a possibly-hung engine, and ctest
# then blocked on it — measured at about 1000s per timeout across 18 runs, perfectly correlated
# with the timeout count. override showed it plainly: 909.87s against a TIMEOUT of 600, passing
# standalone in 23.2s.
#
# stop_engine escalates to SIGKILL after 10s and SAYS SO, so a hang stops being something to
# infer from a sample stack and becomes a line in the run.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
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
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

# Eight HALF-SECOND tones at eight DISTINGUISHABLE frequencies, spaced so no two are confusable
# by a harmonic. Half a second is chosen against both uses below: long enough that the second
# note in the choke and ring patterns lands while the first is still sounding, and short enough
# that the scan can space its notes further apart than the sample and measure each one ALONE.
#
# That second point cost a run: the slots are ONE-SHOTS, so they ignore note-off and play to the
# end. With a one-second sample and notes a quarter-second apart, every measurement window had
# four tones in it and the scan reported the wrong slot for half the keys — a fixture problem
# that read exactly like an addressing bug.
python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
n = sr // 2
freqs = [220.0, 277.0, 330.0, 415.0, 494.0, 587.0, 698.0, 880.0]
for i, f in enumerate(freqs):
    w = wave.open(os.path.join(sys.argv[1], "s%d.wav" % i), 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(b''.join(struct.pack('<h', int(18000 * math.sin(2 * math.pi * f * j / sr)))
                           for j in range(n)))
    w.close()
PY

# An empty project. The kit is built entirely by command.
python3 - "$TMP/blank.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
tr = {"track_id": 0, "name": "K", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "blank"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

SHM="/kitchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 45 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null && break
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load blank --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
# The sleep that was here is redundant: wait_for_boot above returns on the load event,
# and after_command below waits for its own journal line. Neither needed a guess in between.

after_command "$TMP" cli do add-device --track 0 --kind sampler --device-id 1 || true

# ---- LAYS DOWN. One command, eight slots, eight consecutive keys from C-1.
cli do sampler-load --track 0 --device 1 --root 36 \
    --files s0.wav,s1.wav,s2.wav,s3.wav,s4.wav,s5.wav,s6.wav,s7.wav \
    >"$TMP/kit.json" 2>&1 || fail "the kit load exited non-zero: $(cat "$TMP/kit.json")"
sleep 2.0
LOADS="$(grep -c '"event":"sampler.loaded"' "$TMP/eng.log")"
[ "$LOADS" = "8" ] || fail "expected 8 sampler.loaded events, got $LOADS — one --files command
        must lay the whole kit down, or 'drop eight one-shots on a track' is eight commands"
grep '"event":"sampler.render_built"' "$TMP/eng.log" | tail -1 | grep -q '"slots":8' || \
  fail "the device does not have 8 slots:
        $(grep -o '\"event\":\"sampler.render_built\"[^}]*' "$TMP/eng.log" | tail -1)"
grep '"event":"sampler.render_built"' "$TMP/eng.log" | tail -1 | grep -q '"failed":0' || \
  fail "a source failed to decode:
        $(grep -o '\"event\":\"sampler.render_built\"[^}]*' "$TMP/eng.log" | tail -1)"
echo "  lays down: 8 slots on keys 36..43 from one --files command"

# ---- CONFIGURE by command: slots 1 and 2 choke each other, slot 4 rings on repeat.
cli do sampler-slot --track 0 --device 1 --slot 1 --field voice-group --value 1 >/dev/null 2>&1
cli do sampler-slot --track 0 --device 1 --slot 2 --field voice-group --value 1 >/dev/null 2>&1
after_command "$TMP" cli do sampler-slot --track 0 --device 1 --slot 4 --field nna --value 2 
SET="$(grep -c '"event":"sampler.slot_set"' "$TMP/eng.log")"
[ "$SET" = "3" ] || fail "expected 3 sampler.slot_set events, got $SET:
        $(grep -o '\"event\":\"sampler.set_slot_rejected\"[^}]*' "$TMP/eng.log" | tail -3)"
echo "  configures: voice groups and NNA set by command, no JSON"

# ---- THE READ-BACK. A UI cannot draw a kit it cannot see, and until v32 the only way to know
# what slots a sampler had was to save the project and read the file.
#
# PUBLISHED FROM THE SNAPSHOT THE PRODUCER READS, not from the document. That is what gives this
# teeth: a read-back built from the model would answer "what was configured" while the audio
# thread plays something else, and catching exactly that divergence is what a read-back is FOR.
KIT="$(cli get sampler-kit --track 0 --device 1 --seq 7 2>&1)"
echo "$KIT" | python3 -c '
import json, sys
raw = sys.stdin.read()
try:
    d = json.loads(raw)
except Exception:
    print("BADJSON: " + raw[:200]); raise SystemExit(1)
assert d.get("found") is True, d
assert len(d["slots"]) == 8, "expected 8 slots in the read-back, got %d" % len(d["slots"])
assert d["slots_truncated"] == 0, d["slots_truncated"]
by = {s["slot"]: s for s in d["slots"]}
for i in range(1, 9):
    assert by[i]["root"] == 35 + i, (i, by[i]["root"])
    # The SOURCE RESOLVED. A slot whose file is missing is reported as such rather than left to
    # be inferred from a zero length — "silent because the file is missing" and "silent because
    # the sample is empty" are different problems.
    assert by[i]["source_missing"] is False, by[i]
    assert by[i]["length_frames"] > 0, by[i]
# The configuration set by command earlier must be what the ENGINE has, not merely what the
# document says — these two read from different places and this is where they are compared.
assert by[1]["voice_group"] == 1 and by[2]["voice_group"] == 1, "choke group not in the read-back"
assert by[3]["voice_group"] == 0, "an unrelated slot has a voice group"
assert by[4]["nna"] == 2, "NNA=Continue not in the read-back"
assert d["voice_cap"] > 0, d
print("  reads back: 8 slots, choke group and NNA as the ENGINE has them (not as the file says)")
' || fail "the kit read-back did not match: $KIT"

# ---- AND A DEVICE THAT IS NOT THERE ANSWERS "not found" rather than an empty kit. An empty
# answer and a missing device look identical to a caller, and only one of them is a bug.
# ---- THE POLL COUNTER. The kit publishes on REQUEST, so a drawn kit is a snapshot with no way
# to learn it has gone stale. Without something to poll, a UI either re-requests a 2 KB answer on
# a timer or shows a kit that quietly stopped being true — and the second is what actually
# happens, because the timer feels wasteful and gets turned down.
#
# BUMPED ON CHANGE, NOT ON PUBLISH. "Did anyone ask recently" is not the question; "is what I
# drew still right" is. So an edit must move it and a re-read must not.
V1="$(cli get sampler-kit --track 0 --device 1 --seq 21 2>&1 | grep -o '"kit_version": [0-9]*' | grep -o '[0-9]*')"
[ -n "${V1:-}" ] && [ "${V1:-0}" -gt 0 ] || \
  fail "the kit read-back reported no kit_version (got ${V1:-empty}). Zero means an engine that
        does not publish one; the counter starts at 1 precisely so those are distinguishable"
V2="$(cli get sampler-kit --track 0 --device 1 --seq 22 2>&1 | grep -o '"kit_version": [0-9]*' | grep -o '[0-9]*')"
[ "$V1" = "$V2" ] || \
  fail "the kit version changed from $V1 to $V2 without the kit changing — only a REQUEST
        happened in between. A counter that moves when someone asks answers the wrong question
        and makes a polling UI re-fetch forever"
cli do sampler-slot --track 0 --device 1 --slot 1 --field gain-mb --value -300 >/dev/null 2>&1 || true
sleep 0.6
V3="$(cli get sampler-kit --track 0 --device 1 --seq 23 2>&1 | grep -o '"kit_version": [0-9]*' | grep -o '[0-9]*')"
echo "  kit version: $V1 -> $V2 after a re-read -> $V3 after an edit"
python3 -c "
raise SystemExit(0 if $V3 > $V2 else 1)" || \
  fail "editing a slot did not move the kit version ($V2 -> $V3). Then a UI polling it would
        keep drawing the kit it fetched before the edit, which is exactly the staleness the
        counter exists to end"

MISSING="$(cli get sampler-kit --track 0 --device 99 --seq 9 2>&1)"
echo "$MISSING" | grep -q '"found": false' || \
  fail "asking for a device that does not exist should answer found:false, got: $MISSING"
echo "  refuses: a missing device answers found:false, not an empty kit"

after_command "$TMP" cli do save kit --force || true
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

# ---- The saved kit is what we asked for. Read before rendering, so a render failure below is
# about PLAYBACK rather than about the kit never having been built.
python3 - "$TMP/kit.uniproj.json" <<'PYS'
import json, sys
d = json.load(open(sys.argv[1]))
tr = [t for t in d["tracks"] if not t.get("is_master")][0]
dev = [x for x in tr["device_chain"] if x["kind"] == "sampler"][0]
s = dev["sampler"]
assert len(s["slots"]) == 8, len(s["slots"])
assert len(s["sources"]) == 8, len(s["sources"])
by = {x["id"]: x for x in s["slots"]}
for i, slot in enumerate(sorted(s["slots"], key=lambda x: x["id"])):
    assert slot["root_key"] == 36 + i, (slot["id"], slot["root_key"])
    assert slot["key_low"] == slot["key_high"] == slot["root_key"], slot
assert by[1]["voice_group"] == 1 and by[2]["voice_group"] == 1, "choke group not saved"
assert by[3]["voice_group"] == 0, "an unrelated slot was given a voice group"
assert by[4]["nna"] == 2, "NNA=Continue not saved"
print("  saved: 8 fixed-pitch slots on 36..43, choke group on 1+2, NNA=Continue on 4")
PYS

# ---- Now render three patterns and measure WHAT SOUNDED, by frequency.
render() {  # render <projectName> <outName>
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/kitr_$$_$2" \
      ./daw_engine --project "$1" --render "$2" --run-seconds 10 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

# pattern <name> <notes-json> — clones the saved kit and gives it a clip.
pattern() {
  python3 - "$TMP/kit.uniproj.json" "$TMP/$1.uniproj.json" "$2" "$Q" <<'PY'
import json, sys
src, out, notes, Q = sys.argv[1], sys.argv[2], json.loads(sys.argv[3]), int(sys.argv[4])
d = json.load(open(src))
BAR = Q * 4
# FOUR BARS (8 s at 120 bpm), not two. The scan puts its last note at 6 s, and with a 4 s clip
# the transport LOOPED — so keys 41..43 were measured against the loop replaying keys 36..38, and
# the scan reported the wrong slot with high confidence. The clip has to outlast its own content.
clip = {"id": 1, "name": "p", "length": BAR * 4, "kind": "symbolic", "notes": notes}
d["clips"] = [clip]
for t in d["tracks"]:
    if not t.get("is_master"):
        t["placements"] = [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 4,
                            "notes": [], "chords": [], "mutes": []}]
json.dump(d, open(out, "w"))
PY
}

# dominant <wav> <startSec> <endSec> — the strongest frequency in that window, by Goertzel over
# the eight fixture tones. Reports WHICH SLOT sounded, not merely that something did.
dominant() {
  python3 - "$1" "$2" "$3" <<'PYD'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a = int(float(sys.argv[2]) * sr)
b = min(n, int(float(sys.argv[3]) * sr))
freqs = [220.0, 277.0, 330.0, 415.0, 494.0, 587.0, 698.0, 880.0]
best, bestI, total = 0.0, -1, 0.0
for i, f in enumerate(freqs):
    k = 2.0 * math.cos(2.0 * math.pi * f / sr)
    s1 = s2 = 0.0
    for j in range(a, b):
        s0 = s[j * ch] + k * s1 - s2
        s2, s1 = s1, s0
    mag = math.sqrt(max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2))
    total += mag
    if mag > best:
        best, bestI = mag, i
# "which slot" and "how dominant" — a near-tie means two things sounded, which choke must prevent.
print(bestI, int(100.0 * best / total) if total > 0 else 0)
PYD
}

# ---- ADDRESSES. Eight notes, one per key, each must sound as ITS OWN tone.
NOTES='['
for i in $(seq 0 7); do
  # 0.75 s apart at 120 bpm, against a 0.5 s sample: each note finishes before the next begins.
  T=$(( (i + 1) * Q * 3 / 2 ))
  K=$(( 36 + i ))
  NOTES="$NOTES{\"nanotick\":$T,\"duration\":$((Q/4)),\"pitch\":$K,\"velocity\":110,\"column\":0,\"note_id\":$((i+1))},"
done
NOTES="${NOTES%,}]"
pattern scan "$NOTES"
render scan scan
BAD=0
for i in $(seq 0 7); do
  START=$(python3 -c "print((($i+1)*0.75)+0.02)")
  END=$(python3 -c "print((($i+1)*0.75)+0.30)")
  read -r WHICH CONF <<<"$(dominant "$TMP/scan.wav" "$START" "$END")"
  [ "$WHICH" = "$i" ] || { echo "    key $((36+i)) sounded slot $WHICH (conf $CONF%)"; BAD=$((BAD+1)); }
done
[ "$BAD" -eq 0 ] || \
  fail "$BAD of 8 keys played the WRONG slot. Eight copies of one sound would have hidden this
        entirely — the fixture uses eight distinguishable tones precisely so 'the wrong drum
        played' is visible as the wrong frequency"
echo "  addresses: all 8 keys play their own slot, identified by frequency"

# ---- CHOKES. Slots 1 and 2 (keys 36, 37) share voice group 1. Strike 36 then 37 while 36 is
# still sounding: only 37 may remain. Slot 3 (key 38) is ungrouped and must NOT be choked.
pattern choke "[{\"nanotick\":$((Q/2)),\"duration\":$((Q*2)),\"pitch\":36,\"velocity\":110,\"column\":0,\"note_id\":1},{\"nanotick\":$((Q)),\"duration\":$((Q*2)),\"pitch\":37,\"velocity\":110,\"column\":1,\"note_id\":2}]"
render choke choke
read -r W1 C1 <<<"$(dominant "$TMP/choke.wav" 0.30 0.45)"
[ "$W1" = "0" ] || fail "before the choke, the first slot should dominate; got slot $W1"
# THE WINDOW IS THE OVERLAP, 0.55..0.72 s, AND THAT IS THE WHOLE ASSERTION. The first note runs
# 0.25..0.75 s and the second 0.50..1.00 s, so only 0.50..0.75 has both. Measured over
# 0.60..0.90 the second note dominates at 66% even with CHOKE COMPLETELY DISABLED, purely
# because the first has ended by 0.75 — which is what the negative control caught: the check
# passed with the feature removed. Inside the overlap the two are ~50/50 unchoked and ~100/0
# choked, so the numbers separate instead of overlapping.
read -r W2 C2 <<<"$(dominant "$TMP/choke.wav" 0.55 0.72)"
[ "$W2" = "1" ] || fail "after the choke the SECOND slot should dominate; got slot $W2"
[ "$C2" -gt 85 ] || \
  fail "inside the overlap the second slot is only $C2% of the energy — the first is still
        sounding, so the voice group did not cut it. Open hat and closed hat is exactly this.
        (Unchoked this reads about 50%; choked it reads about 100%.)"
echo "  chokes: same voice group cuts (second slot is ${C2}% of the energy after)"

pattern nochoke "[{\"nanotick\":$((Q/2)),\"duration\":$((Q*2)),\"pitch\":36,\"velocity\":110,\"column\":0,\"note_id\":1},{\"nanotick\":$((Q)),\"duration\":$((Q*2)),\"pitch\":38,\"velocity\":110,\"column\":1,\"note_id\":2}]"
render nochoke nochoke
read -r W3 C3 <<<"$(dominant "$TMP/nochoke.wav" 0.55 0.72)"
[ "$C3" -lt 70 ] || \
  fail "an UNGROUPED slot was choked too (one slot is ${C3}% of the energy). If voice group 0
        chokes voice group 0, an entire kit is monophonic — the sentinel-collides-with-a-legal-
        value bug, in the place it would hurt most"
echo "  and does not over-choke: an ungrouped slot keeps ringing alongside (${C3}% dominant)"

# ---- RINGS. Slot 4 is KEY 39 — slots are 1-indexed from the base key, so slot N is key 35+N.
# Slot 4 was given NNA=Continue above, so striking key 39 twice in one column must leave BOTH
# voices sounding; key 40 (slot 5, default Cut) is the control.
#
# Getting this pair the wrong way round is what the first run did, and it is worth noting that
# the measurement was RIGHT while the labels were wrong: the Continue slot really was louder,
# it was just being called the Cut one.
#
# THE SECOND NOTE IS QUIET (velocity 30 against 110) AND THAT IS THE WHOLE MEASUREMENT.
# Two strikes at the same velocity are two copies of ONE tone at a fixed phase offset, so they
# partially cancel — Continue came out only 18% louder than Cut, which is real but far too close
# to a physics accident to assert on. With a loud first strike and a quiet second, Cut leaves
# only the quiet voice while Continue leaves loud+quiet, and the two differ by a factor rather
# than by a margin. The window is the OVERLAP (0.55..0.72 s), where the first voice is still
# sounding in the Continue case and already gone in the Cut case.
pattern ring "[{\"nanotick\":$((Q/2)),\"duration\":$((Q*2)),\"pitch\":39,\"velocity\":110,\"column\":0,\"note_id\":1},{\"nanotick\":$((Q)),\"duration\":$((Q*2)),\"pitch\":39,\"velocity\":30,\"column\":0,\"note_id\":2}]"
render ring ring
pattern cut "[{\"nanotick\":$((Q/2)),\"duration\":$((Q*2)),\"pitch\":40,\"velocity\":110,\"column\":0,\"note_id\":1},{\"nanotick\":$((Q)),\"duration\":$((Q*2)),\"pitch\":40,\"velocity\":30,\"column\":0,\"note_id\":2}]"
render cut cut
energy() {
  python3 - "$1" "$2" "$3" <<'PYE'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(float(sys.argv[2]) * sr), min(n, int(float(sys.argv[3]) * sr))
print(int(sum(abs(s[j * ch]) for j in range(a, b)) / max(1, b - a)))
PYE
}
RING="$(energy "$TMP/ring.wav" 0.55 0.72)"
CUT="$(energy "$TMP/cut.wav" 0.55 0.72)"
[ "$RING" -gt "$((CUT * 2))" ] || \
  fail "NNA=Continue ($RING) is not louder than NNA=Cut ($CUT) after a repeated note. Continue
        must leave the PREVIOUS voice sounding — an arpeggiated chord or a ringing 808 down one
        column, which is the gesture a truncate-on-entry model cannot express at all"
echo "  rings: NNA=Continue keeps the previous note ($RING vs $CUT for Cut)"

echo "sampler_kit_check: PASS — a kit lays down, addresses, chokes and rings"
