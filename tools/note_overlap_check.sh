#!/usr/bin/env bash
# ENTERING A NOTE OVER A SOUNDING ONE NO LONGER HAS TO DESTROY IT.
#
# `addNoteToClip` truncated the sounding note in the same column UNCONDITIONALLY, and it did it
# IN THE DOCUMENT: the length the player typed was gone at entry, and no later view could recover
# it. docs/TRACKER_GAP_LIST.md item 1 calls it the only thing on that list actively losing work.
#
# Opcode 93 makes it a per-lane policy. OFF by default, so every project written before this
# behaves exactly as it did.
#
# NOTHING IN PLAYBACK CHANGED. The scheduler already honours overlapping durations in one column
# — that was measured before this was built, not assumed — so the whole feature is a decision not
# to truncate.
#
# SIX PROPERTIES:
#   DEFAULT OFF   a fresh track still truncates. The old behaviour is the default and is checked,
#                 because a flag that silently changes what every existing project does is not a
#                 flag, it is a regression
#   REACHES       allow_note_overlap reads back as what was sent
#   KEEPS         with it ON, the first note keeps the duration it was AUTHORED with
#   PERSISTS      the flag survives a save and a reload
#   AUDIBLE       and the overlap is real: both notes ring, measured as the POWER SUM of the two
#                 played alone. This is the property the other five are in service of
#   THE TRAP      the same render with the sampler slot back at nna 0 (Cut) sounds like ONE note.
#                 That is the silent false negative this check exists to pin down: turn the lane
#                 flag on, test on a default kit, hear no difference, conclude it does not work
#
# Needs a real audio device for the interactive half (non-test mode); the audible half is offline.
#   tools/note_overlap_check.sh
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

# The slot carries nna 2 = CONTINUE. On the default (0 = Cut) the sampler cuts its own previous
# voice one layer below the scheduler, so the audible half below would measure one note however
# correct this feature was. The last property renders the nna 0 case deliberately, so that trap is
# pinned down rather than merely avoided.
mkfixture() {  # mkfixture <path> <nna>
  python3 - "$1" "$2" "$Q" <<'PY'
import json, sys, os, wave, struct, math
out, nna, Q = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
d = os.path.dirname(out)
sr = 48000
w = wave.open(os.path.join(d, "s.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(12000 * math.sin(2*math.pi*220*i/sr)))
                       for i in range(sr*2)))
w.close()
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id":1,"name":"t","source_local_id":1,"slice_id":0,"start_frame":0,"end_frame":0,
        "loop_start_frame":0,"loop_end_frame":0,"loop_xfade_frames":0,"loop_mode":0,
        "sustain_loop":0,"key_low":0,"key_high":127,"root_key":60,"pitch_track_milli":1000,
        "tune_cents":0,"vel_low":0,"vel_high":127,"layer_group":0,"select_mode":0,"gate":1,
        "reverse":0,"gain_millibels":0,"pan_thousandths":0,"voice_group":0,"nna":nna,
        "polyphony":0,"choke_fade_us":3000,"mod_set_id":1,"output_stem":0,"quality":1}
dev = {"device_id":1,"kind":"sampler","capability_mask":5,"patcher_node_id":0,
       "host_slot_index":0,"bypass":False,
       "sampler":{"next_slot_id":2,"next_source_id":2,"next_mod_set_id":2,"stem_count":0,
                  "voice_cap":16,"default_view":0,
                  "sources":[{"local_id":1,"path":"s.wav","content_key":0}],
                  "slice_sets":[],
                  "mod_sets":[{"id":1,"name":"d","filter_type":0,"cutoff_milli":1000,
                               "resonance_milli":0,"next_modulator_id":1,"modulators":[]}],
                  "slots":[slot]}}
tr = {"track_id":0,"name":"S","harmony_quantize":False,"lines_per_beat":4,
      "mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},
      "routing":{"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),
                 "pre_fader_send":True},
      "device_chain":[dev],"mod_links":[],
      "placements":[{"clip_id":1,"id":1,"at":0,"length":BAR,"notes":[],"chords":[],"mutes":[]}]}
json.dump({"schema_version":4,"meta":{"name":os.path.basename(out).split('.')[0]},
           "nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],
           "harmony_timeline":[],
           "clips":[{"id":1,"name":"p","length":BAR,"kind":"symbolic","notes":[]}],
           "tracks":[tr]}, open(out,"w"))
PY
}
mkfixture "$TMP/ov.uniproj.json" 2

SHM="/novchk_$$"
# `exec`, so $! is the ENGINE and kill reaches it rather than the subshell around it.
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project ov --run-seconds 50 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

# The duration of the note at tick 0, which is the one the second entry either truncates or does
# not. Read from the ENGINE's published clip, not from the file, so this is what the app has.
dur0() {
  cli get notes --track 0 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print('unreadable'); raise SystemExit
for n in d.get('notes', []):
    if n['nanotick'] == 0:
        print(n['duration']); break
else:
    print('missing')
" 2>/dev/null
}
flag() {
  cli get tracks 2>/dev/null | grep -o '"allow_note_overlap": [a-z]*' | head -1 |
    sed 's/.*: //'
}

# How many notes the engine has published for this track. The pair below is entered in ORDER and
# each entry is waited for, which is the whole correctness argument: the second note truncates
# "whatever is sounding here", so if it arrives while the first has not landed there is nothing
# to truncate and the take is silently wrong rather than late.
notecount() {
  cli get notes --track 0 2>/dev/null | grep -oE '"note_count": [0-9]+' | grep -oE '[0-9]+$'
}
# THE CLIP VERSION, WHICH IS THE FACT THE NEXT WRITER ACTUALLY READS. An edit carries the version
# it was written against and is REFUSED if that has moved, and each `daw-cli do` is a fresh process
# that reads the version when it starts. So waiting for the NOTE COUNT is waiting on the wrong
# published fact: count and version are published on their own ticks, and in the window where the
# count has advanced but the version has not, the next writer reads a stale base and its write is
# correctly refused. That is what happened — history.jsonl from the failing run reads
# `write_note received` then `write_note rejected:version`, and wait_notes then spent its whole
# 20s budget waiting for a second note that was never going to arrive.
#
# The comment on the deletes below already worked this out for THAT gesture. This one had the same
# hazard and a wait that could not see it.
trackver() {
  cli get tracks 2>/dev/null \
    | sed -n 's/.*"track_id": *0,.*"clip_version": *\([0-9][0-9]*\).*/\1/p' | head -1
}
wait_notes() {  # wait_notes <want> <what> [version-before]
  local want_ver="${3:-}"
  for _ in $(seq 1 80); do
    if [ "$(notecount)" = "$1" ]; then
      # Both, when a base version was supplied: the count says the edit is visible, the version
      # says the NEXT edit will be accepted.
      [ -z "$want_ver" ] && return 0
      [ "$(trackver)" != "$want_ver" ] && return 0
    fi
    sleep 0.25
  done
  fail "$2: the engine published $(notecount) notes, waited 20s for $1 (clip_version $(trackver)).
        If the count is short by one, look for \"rejected:version\" in history.jsonl before
        blaming slowness: a refused edit never arrives however long this waits."
}

enter_pair() {  # a 4-quarter note at 0, then a note one quarter in — the gesture under test
  # WAITS FOR EACH ENTRY, rather than sleeping a fixed time. This check failed once in four runs
  # under `ctest -j8` with two 0.6s sleeps here: the machine was loaded enough that an edit had
  # not landed before the next command was sent, and the second note then truncated nothing. A
  # check that is right three times in four is worse than no check, because the one failure reads
  # as a real defect in note entry.
  local v0 v1
  v0="$(trackver)"
  # A GUARD THAT CANNOT SILENTLY DISABLE ITSELF. wait_notes treats an empty base version as "no
  # version condition", so if trackver ever stopped parsing — a renamed field, a reordered line —
  # this would quietly go back to waiting on the count alone and the flake would return wearing
  # the same green tick it wore before. Assert the value exists rather than trust the parse.
  case "${v0:-}" in
    ''|*[!0-9]*) fail "trackver parsed '$v0' from \`get tracks\`, which is not a version. The
        version wait below would silently do nothing and this check would flake again." ;;
  esac
  cli do note --track 0 --nanotick 0 --pitch 60 --duration $((Q*4)) --column 0 >/dev/null 2>&1
  wait_notes 1 "after entering the first note" "$v0"
  v1="$(trackver)"
  cli do note --track 0 --nanotick $Q --pitch 67 --duration $((Q*3)) --column 0 >/dev/null 2>&1
  wait_notes 2 "after entering the second note" "$v1"
}

# ---- DEFAULT OFF. The old behaviour is the default, and that is checked rather than assumed: a
# flag that silently changes what every existing project does is a regression wearing a feature's
# clothes.
[ "$(flag)" = "false" ] || fail "allow_note_overlap defaults to '$(flag)', expected false"
enter_pair
D_OFF="$(dur0)"
[ "$D_OFF" = "$Q" ] || \
  fail "with overlap OFF the first note has duration '$D_OFF', expected $Q — it should have been
        truncated to where the second note starts. If this is $((Q*4)) the default has changed and
        every existing project now behaves differently"
echo "  default off: the first note was truncated to $D_OFF, as it always has been"

# ---- REACHES.
cli do note-overlap --track 0 --on 1 >/dev/null 2>&1
for _ in $(seq 1 40); do [ "$(flag)" = "true" ] && break; sleep 0.25; done
[ "$(flag)" = "true" ] || \
  fail "allow_note_overlap never read back as true, it stayed '$(flag)'. Either opcode 93 did not
        reach the runtime or the publisher is not reading the atomic it writes"
echo "  reads back as true"

# ---- KEEPS. Same gesture, same track, flag flipped.
# ONE DELETE AT A TIME, EACH WAITED FOR — and this is about the engine's concurrency control, not
# about speed. An edit carries the clip version it was written against and is REFUSED if that has
# moved (requireMatchingClipVersion). Each `daw-cli do` is a fresh process that reads the version
# when it starts, so firing both deletes back to back lets the second read its base BEFORE the
# first has applied: the second is then correctly refused, and the clip keeps a note.
#
# It failed exactly that way once under `ctest -j8` — "published 1 notes, waited 20s for 0", which
# is a refusal rather than slowness, and no amount of waiting at the end would have fixed it.
cli do delete-note --track 0 --nanotick 0 --pitch 60 --column 0 >/dev/null 2>&1
wait_notes 1 "after deleting the first note"
cli do delete-note --track 0 --nanotick $Q --pitch 67 --column 0 >/dev/null 2>&1
wait_notes 0 "after deleting both notes"
enter_pair
D_ON="$(dur0)"
[ "$D_ON" = "$((Q*4))" ] || \
  fail "with overlap ON the first note has duration '$D_ON', expected $((Q*4)) — the length it was
        AUTHORED with. It is still being truncated at entry, which is the data loss this whole
        feature exists to stop"
echo "  overlap on: the first note kept its authored duration $D_ON"

# ---- PERSISTS, and comes back.
cli do save ovout --force >/dev/null 2>&1 || true
for _ in $(seq 1 40); do [ -f "$TMP/ovout.uniproj.json" ] && break; sleep 0.25; done
[ -f "$TMP/ovout.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"
grep -q '"allow_note_overlap": *true' "$TMP/ovout.uniproj.json" || \
  fail "allow_note_overlap is not true in the saved project, so the lane comes back cutting notes
        again on reload"
# MOVED AWAY FIRST: reloading straight after saving cannot tell "the load restored true" from
# "nothing happened and it was still true".
cli do note-overlap --track 0 --on 0 >/dev/null 2>&1
for _ in $(seq 1 40); do [ "$(flag)" = "false" ] && break; sleep 0.25; done
[ "$(flag)" = "false" ] || fail "could not turn the flag back off before the reload test"
cli do load ovout --force >/dev/null 2>&1
for _ in $(seq 1 40); do [ "$(flag)" = "true" ] && break; sleep 0.25; done
[ "$(flag)" = "true" ] || \
  fail "after reloading the saved project the flag reads '$(flag)', and the file says true. The
        value was turned OFF first, so this is the load dropping it rather than nothing happening"
echo "  persists: saved true, turned off, reloaded, true again"
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

# ---- AUDIBLE, offline. The point of all of the above: two notes that overlap must SOUND like two.
#
# Rendered from the project the engine just saved, so what is measured is what the edits produced
# — not a fixture hand-written to look like them.
render() {  # render <name>
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/novr_${$}_$1" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --project "$1" --render "$1" --run-seconds 5 --block-size 256 \
      >"$TMP/$1.log" 2>&1 ) || fail "the '$1' render exited non-zero"
  [ -s "$TMP/$1.wav" ] || fail "the '$1' render wrote no output"
}
# The saved project, and the same document with each note ALONE, so the overlap can be compared
# against the two levels it should be the power sum of.
python3 - "$TMP/ovout.uniproj.json" "$TMP" <<'PY'
import json, sys, os, copy
src, d = sys.argv[1], sys.argv[2]
doc = json.load(open(src))
notes = doc["clips"][0]["notes"]
assert len(notes) == 2, "expected the two entered notes, got %d" % len(notes)
notes.sort(key=lambda n: n["nanotick"])
for name, keep in (("solo1", [notes[0]]), ("solo2", [notes[1]])):
    one = copy.deepcopy(doc)
    one["meta"]["name"] = name
    one["clips"][0]["notes"] = keep
    json.dump(one, open(os.path.join(d, name + ".uniproj.json"), "w"))
PY
render ovout
render solo1
render solo2

rms() {  # rms <name> — over the overlap window, one quarter in to just before the first note ends
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb'); ch, nf, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (nf * ch), w.readframes(nf)); w.close()
seg = [s[i * ch] for i in range(int(1.1 * sr), min(int(1.9 * sr), nf))]
print(int((sum(v * v for v in seg) / max(1, len(seg))) ** 0.5))
PY
}
A="$(rms solo1)"; B="$(rms solo2)"; BOTH="$(rms ovout)"
echo "  overlap window — note1 alone: $A, note2 alone: $B, both: $BOTH"
[ "${A:-0}" -gt 0 ] && [ "${B:-0}" -gt 0 ] || \
  fail "one of the notes is silent on its own ($A, $B), so the sum below is vacuous"
# The power sum, within 10%. Two uncorrelated tones add in power, not amplitude — asserting
# "louder than either" would pass on a render where only the louder one survived.
python3 -c "
import math, sys
want = math.sqrt($A*$A + $B*$B)
got = $BOTH
raise SystemExit(0 if abs(got - want) < 0.10 * want else 1)" || \
  fail "both notes do not ring: the overlap measures $BOTH, and two uncorrelated tones at $A and
        $B sum in POWER to about $(python3 -c "import math; print(int(math.sqrt($A*$A+$B*$B)))").
        A value near $B alone means the first note was cut after all"
echo "  audible: the overlap is the power sum, so both notes are sounding"

# ---- THE TRAP, pinned down rather than avoided. Same document, same flag, sampler slot back at
# nna 0 (Cut) — which is the DEFAULT. If this did not sound different, the audible assertion above
# would not be measuring the feature.
python3 - "$TMP/ovout.uniproj.json" "$TMP/trap.uniproj.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
doc["meta"]["name"] = "trap"
for t in doc["tracks"]:
    for dev in t.get("device_chain", []):
        for slot in dev.get("sampler", {}).get("slots", []):
            slot["nna"] = 0
json.dump(doc, open(sys.argv[2], "w"))
PY
render trap
TRAP="$(rms trap)"
echo "  same document, slot nna back to 0 (Cut): $TRAP"
python3 -c "
raise SystemExit(0 if abs($TRAP - $B) < 0.10 * $B else 1)" || \
  fail "with the slot at nna 0 the overlap measures $TRAP, expected about $B — one note. If this
        now sounds like two, the slot's own NNA is being ignored and the audible assertion above
        was not testing what it claims"
echo "  the trap is real: with a default (Cut) slot the same document sounds like ONE note"

echo "note_overlap_check: PASS — the lane can keep the note above, and it is audible when it does"
