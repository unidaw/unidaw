#!/usr/bin/env bash
# A CLIP'S NAME AND ITS AUDIO SOURCE PATH ARE SETTABLE — AND SETTING ONE IS SEEN, HEARD AND SAVED.
#
# These were the LAST TWO GAPs in persisted_field_reach: both persisted by the project format, both
# published (name in UiClipExtent, path in UiAudioSource), the path rendered — and no command could
# write either. A UI could draw a clip's name and never change it; an audio clip could not be
# repointed at another file without a text editor. Both were GAPs for one reason, and it was not
# that anybody decided against them: a string does not fit the 40-byte ring payload. Opcode 98
# rides the BulkChunk carrier (83), which is why it could be built at all.
#
# THE TWO FIELDS DO NOT SHARE A PUBLISHER, which is the whole reason this check asserts both rather
# than treating them as one command with a parameter. A name reaches readers through
# rebuildFlatAndPublish; a path reaches them through rebuildAudioRender plus publishAudioClipTable.
# A handler calling one publisher for both saves correctly and shows nothing.
#
# TWO DEFECTS FOUND WHILE BUILDING IT, both by running the thing rather than reading it:
#   1. The retarget moved the clip's sourceId to the newly interned source and the published
#      sources ARRAY still listed only what the load had seen — so the table carried a clip
#      pointing at a sourceId that was not in it. A DANGLING JOIN is worse than the stale path it
#      replaced: a reader gets no waveform, no path, and nothing saying why. The source loop was
#      load-only, under a comment reading "these change only at load", directly beneath a comment
#      explaining that a load-only copy of the CLIP loop is how that half became a load-time
#      snapshot. Property JOIN exists for this and nothing else.
#   2. daw-cli defaulted a missing --clip to 0, so a typo renamed whichever clip was at 0.
#
# EIGHT PROPERTIES:
#   SEEN       a rename shows up in the published clip extents, without a save
#   SAVED      and in the project file, which is where the GAP was declared
#   HEARD      a retarget CHANGES THE RENDER — 440 Hz becomes 880 Hz through save + offline
#              render. Asserted on audio, never on the field: a path that updates while the render
#              does not is precisely the bug worth catching
#   JOIN       the published sources table gains the new source AND the clip's sourceId resolves
#              into it (see defect 1)
#   ADDRESSED  two audio clips, rename one and the other is untouched — a fixture with one clip
#              cannot tell a command that addresses correctly from one that edits whatever it
#              finds first
#   MULTICHUNK a path longer than one 32-byte chunk arrives whole, which is the entire reason this
#              opcode exists rather than a scalar one
#   REFUSED    a path that does not resolve, a source_path on a SYMBOLIC clip, an oversize name,
#              and a clip id that is not there. Every one asserted through the ENGINE's log, not
#              daw-cli's exit code — the CLI validates SHAPE and the engine validates DOMAIN,
#              because the web UI's sidecar writes the ring directly and never runs daw-cli
#   INTACT     after a refused retarget the render is UNCHANGED — a refusal that half-applies is
#              worse than one that does nothing
#
# No audio device needed: the render assertions are offline.
#   tools/clip_text_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# TWO TONES AN OCTAVE APART, so "the render changed" is a specific measurable claim (the dominant
# bin moved to exactly 880) rather than "it sounds different", which would pass on an engine that
# rendered noise or the wrong file.
tone() {  # tone <path> <hz>
  python3 - "$1" "$2" <<'PY'
import sys, wave, struct, math
sr, dur, f = 44100, 2.0, float(sys.argv[2])
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(0.6*32767*math.sin(2*math.pi*f*i/sr)))
                       for i in range(int(sr*dur))))
w.close()
PY
}
tone "$TMP/a.wav" 440
tone "$TMP/b.wav" 880

# A LONG FILENAME ON PURPOSE. The header is 20 bytes and each chunk carries 32, so a path of this
# length spans several chunks — the multi-chunk path is what separates this opcode from a scalar
# one, and a fixture with a short path would never exercise it.
LONGDIR="$TMP/a_deliberately_long_directory_name_to_span_several_bulk_chunks"
mkdir -p "$LONGDIR"
cp "$TMP/b.wav" "$LONGDIR/b.wav"

# TWO AUDIO CLIPS AND ONE SYMBOLIC. Ids 7 and 9, written 9-then-7 so file order and id order
# disagree, and neither is 1 — an id equal to an index or a count cannot pass by coincidence.
python3 - "$TMP/ct.uniproj.json" "$Q" "$TMP/a.wav" <<'PY'
import json, sys
out, Q, wav = sys.argv[1], int(sys.argv[2]), sys.argv[3]
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def audio(cid, name):
    return {"id": cid, "name": name, "length": BAR, "kind": "audio",
            "audio": {"source_path": wav, "source_start_frame": 0, "gain_db": 0.0,
                      "fade_in": 0, "fade_out": 0}}
sym = {"id": 5, "name": "sym", "length": BAR, "kind": "symbolic",
       "lines_per_beat": 4, "time_sig_numerator": 4, "time_sig_denominator": 4, "notes": []}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 7, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 9, "id": 2, "at": 2 * BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 5, "id": 3, "at": 4 * BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "ct"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [audio(9, "nine"), audio(7, "seven"), sym], "tracks": [tr]}, open(out, "w"))
PY

# The BASELINE render, before any edit — the control the HEARD and INTACT properties compare to.
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" ./daw_engine --project ct --render base \
    --run-seconds 3 >"$TMP/base.log" 2>&1 )
[ -s "$TMP/base.wav" ] || fail "the baseline render produced no file — every audio assertion below
        would be comparing nothing to nothing (see $TMP/base.log)"

dominant() {  # dominant <wav>  -> "<hz> <peak>"
  python3 - "$1" <<'PY'
import sys, wave, numpy as np
w = wave.open(sys.argv[1], 'rb'); ch = w.getnchannels(); sr = w.getframerate()
d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
if ch > 1:
    d = d.reshape(-1, ch)[:, 0]
d = d[:sr]
if d.size == 0 or np.abs(d).max() < 1:
    print("0 0.000"); raise SystemExit
sp = np.abs(np.fft.rfft(d * np.hanning(len(d))))
f = np.fft.rfftfreq(len(d), 1.0 / sr)
print("%d %.3f" % (int(round(f[int(np.argmax(sp))])), np.abs(d).max() / 32768.0))
PY
}
BASE="$(dominant "$TMP/base.wav")"
[ "${BASE%% *}" = "440" ] || fail "the baseline renders at ${BASE%% *} Hz, not 440 — the fixture is
        not what this check thinks it is, so nothing below would mean anything"
echo "  baseline renders at 440 Hz (peak ${BASE##* })"

SHM="/cliptext_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project ct --run-seconds 120 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

extname() {  # extname <clipId> -> the published name, or "missing"
  cli get extents 2>/dev/null | python3 -c "
import json, sys
want = $1
try:
    rows = json.load(sys.stdin)
except Exception:
    print('unparseable'); raise SystemExit
for e in rows:
    if e.get('clip') == want:
        print(e.get('name', 'nokey')); raise SystemExit
print('missing')
" 2>/dev/null
}
# The clip's sourceId AND whether that id is present in the sources array — the join, not just the
# field. Printed together so a broken join cannot be read as a working one.
joins() {  # joins <clipId> -> "<sourceId> <resolves:yes|no> <path>"
  cli get audio-sources 2>/dev/null | python3 -c "
import re, sys
want = $1
txt = sys.stdin.read()
srcs = {}
for line in txt.splitlines():
    m = re.search(r'\"sourceId\": (\d+).*?\"path\": \"([^\"]*)\"', line)
    if m:
        srcs[int(m.group(1))] = m.group(2)
for line in txt.splitlines():
    if '\"clipId\"' not in line:
        continue
    m = re.search(r'\"clipId\": (\d+), \"sourceId\": (\d+)', line)
    if m and int(m.group(1)) == want:
        sid = int(m.group(2))
        print('%d %s %s' % (sid, 'yes' if sid in srcs else 'no', srcs.get(sid, '-')))
        raise SystemExit
print('missing no -')
" 2>/dev/null
}
reason() {  # reason <event> -> last matching reason string
  grep "\"event\":\"$1\"" "$TMP/eng.log" 2>/dev/null | tail -1 |
    python3 -c "import re,sys; t=sys.stdin.read(); m=re.search(r'\"reason\":\"([^\"]*)\"', t); print(m.group(1) if m else 'none')"
}
waitname() {  # waitname <clipId> <want>
  for _ in $(seq 1 60); do
    [ "$(extname "$1")" = "$2" ] && return 0
    sleep 0.25
  done
  return 1
}

ok=1
# EVERY SUCCESS LINE IS GUARDED BY THE PROPERTY IT DESCRIBES. The first version printed them
# unconditionally, so a negative control produced "FAIL: ..." immediately followed by the line
# claiming that same property held. Two of the three controls below surfaced it. A reader skimming
# a red run for what still works is exactly who that misleads.
say() {  # say <flagvar> <text> — print only if the flag is still 1
  [ "$(eval "echo \$$1")" = "1" ] && echo "  $2"
  return 0
}
[ "$(extname 7)" = "seven" ] || fail "clip 7 publishes '$(extname 7)', not its fixture name 'seven'
        — the SEEN property below would prove nothing"
[ "$(extname 9)" = "nine" ] || fail "clip 9 publishes '$(extname 9)', not 'nine'"
echo "  both clips publish their fixture names"

# ---- SEEN. UiClipExtent::name has carried the name since the region existed and no command could
# change it; rt.clipExtents is derived inside rebuildFlatAndPublish, so a handler that bumped a
# version instead of re-deriving would republish the OLD name.
cli do clip-name --track 0 --clip 7 --name "renamed" >/dev/null 2>&1
waitname 7 "renamed" || \
  fail "clip 7 still publishes '$(extname 7)' after a rename. The model may well have changed and
        the save may well be correct — the extents are DERIVED, so a publish that does not re-derive
        reports the old answer, which is worse than having no read-back at all"
echo "  seen: clip 7 publishes 'renamed'"

# ---- ADDRESSED. The handler edits the first id match and the runtime's clip order is not the
# file's, so a fixture with one clip cannot tell a correct address from no address at all.
[ "$(extname 9)" = "nine" ] || \
  fail "renaming clip 7 also changed clip 9 to '$(extname 9)' — the command is editing by position
        or by whatever it finds first, not by id"
echo "  addressed: clip 9 was left alone"

# ---- REFUSED, all four, each through the ENGINE's own rejection rather than daw-cli's exit code.
# `cli ... && fail` would assert daw-cli's copy of the rule and pass with the engine's guard
# deleted, which is how three shipped commands ended up with their engine-side guards unexercised.
refok=1
cli do clip-name --track 0 --clip 4242 --name "ghost" >/dev/null 2>&1
sleep 0.6
[ "$(reason clip_text.rejected)" = "no_such_clip" ] || \
  { echo "  FAIL: naming a clip that does not exist was not refused with no_such_clip (got
        '$(reason clip_text.rejected)')"; ok=0; refok=0; }

cli do clip-source --track 0 --clip 5 --path "$TMP/b.wav" >/dev/null 2>&1
sleep 0.6
[ "$(reason clip_text.rejected)" = "not_an_audio_clip" ] || \
  { echo "  FAIL: a source_path on a SYMBOLIC clip was not refused with not_an_audio_clip (got
        '$(reason clip_text.rejected)'). Accepting it writes a field the save path never emits for
        this kind and the renderer never reads — succeeded, and nothing happened"; ok=0; refok=0; }

# --oversize-anyway is what makes this reachable at all: without it daw-cli refuses first and the
# ENGINE's limit is an untested claim about another process.
LONGNAME="$(python3 -c "print('x'*40)")"
cli do clip-name --track 0 --clip 7 --name "$LONGNAME" --oversize-anyway >/dev/null 2>&1
sleep 0.6
[ "$(reason clip_text.rejected)" = "text_too_long" ] || \
  { echo "  FAIL: a 40-byte name was not refused with text_too_long (got
        '$(reason clip_text.rejected)'). The published field holds 32, and the engine must refuse
        rather than shorten, or a read-back disagrees with the write for every long name"; ok=0; refok=0; }
[ "$(extname 7)" = "renamed" ] || \
  { echo "  FAIL: the refused oversize name still changed the published name to '$(extname 7)'";
    ok=0; refok=0; }

cli do clip-source --track 0 --clip 7 --path "$TMP/definitely_absent.wav" >/dev/null 2>&1
sleep 0.6
[ "$(reason clip_text.rejected)" = "source_unreadable" ] || \
  { echo "  FAIL: a retarget at a nonexistent file was not refused with source_unreadable (got
        '$(reason clip_text.rejected)'). Accepting it leaves a clip that displays the new path and
        renders silence"; ok=0; refok=0; }
say refok "refused: no_such_clip, not_an_audio_clip, text_too_long, source_unreadable"

# ---- INTACT. A refusal that half-applies is worse than one that does nothing, and the only way to
# see it is to render again after the refusal rather than to re-read the field.
cli do save ct_refused >/dev/null 2>&1
sleep 1
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" ./daw_engine --project ct_refused --render refused \
    --run-seconds 3 >"$TMP/refused.log" 2>&1 )
intactok=1
REF="$(dominant "$TMP/refused.wav" 2>/dev/null || echo "0 0.000")"
[ "${REF%% *}" = "440" ] || \
  { echo "  FAIL: after the REFUSED retarget the render is at ${REF%% *} Hz, not the original 440
        — the refusal applied something"; ok=0; intactok=0; }
say intactok "intact: a refused retarget left the render at 440 Hz"

# ---- MULTICHUNK + HEARD + JOIN. The long path spans several 32-byte chunks.
B0="$(joins 7)"
cli do clip-source --track 0 --clip 7 --path "$LONGDIR/b.wav" >/dev/null 2>&1
sleep 1.5
mcok=1
CHUNKS="$(grep '"event":"bulk.assembled"' "$TMP/eng.log" 2>/dev/null | tail -1 |
          python3 -c "import re,sys; t=sys.stdin.read(); m=re.search(r'\"chunks\":(\d+)', t); print(m.group(1) if m else '0')")"
[ "${CHUNKS:-0}" -gt 1 ] || \
  { echo "  FAIL: the long path assembled from $CHUNKS chunk(s) — the multi-chunk path is the
        entire reason this command rides the carrier, and a single-chunk fixture never tests it";
    ok=0; mcok=0; }
say mcok "multichunk: the path arrived in $CHUNKS chunks"

B1="$(joins 7)"
joinok=1
case "$B1" in
  *" yes "*) ;;
  *) echo "  FAIL: clip 7 now points at source '${B1%% *}' and that id is NOT in the published
        sources array ($B1). A reader joining clipId -> sourceId gets no waveform, no path, and
        nothing saying why — the source half of this table used to be written only at load"
     ok=0; joinok=0 ;;
esac
[ "${B1%% *}" != "${B0%% *}" ] || \
  { echo "  FAIL: the retarget did not move clip 7's sourceId (still ${B0%% *})"; ok=0; joinok=0; }
# GUARDED, because the first version printed this line unconditionally — so the negative control
# that broke the join reported the FAIL and then said the join was fine, one line apart. A success
# line that survives its own property's failure is how a reader talks themselves out of a red run.
say joinok "join: clip 7 moved ${B0%% *} -> ${B1%% *} and that source is published"

cli do save ct_rt >/dev/null 2>&1
sleep 1
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" ./daw_engine --project ct_rt --render after \
    --run-seconds 3 >"$TMP/after.log" 2>&1 )
heardok=1
AFTER="$(dominant "$TMP/after.wav" 2>/dev/null || echo "0 0.000")"
[ "${AFTER%% *}" = "880" ] || \
  { echo "  FAIL: after the retarget the render is at ${AFTER%% *} Hz, wanted 880. The field may
        read back correctly and the file may be right — if the render did not follow, the clip
        displays one file and plays another"; ok=0; heardok=0; }
say heardok "heard: the render moved 440 Hz -> ${AFTER%% *} Hz"

# ---- SAVED. Where the GAP was declared: the format persists both, so a command that does not
# reach the file has not closed it.
python3 - "$TMP/ct_rt.uniproj.json" <<'PY' || ok=0
import json, sys
doc = json.load(open(sys.argv[1]))
clips = {c["id"]: c for c in doc["clips"]}
bad = 0
if clips[7].get("name") != "renamed":
    print("  FAIL: the saved project has clip 7 named %r, wanted 'renamed'" % clips[7].get("name"))
    bad = 1
if not clips[7]["audio"]["source_path"].endswith("b.wav"):
    print("  FAIL: the saved project still points clip 7 at %r"
          % clips[7]["audio"]["source_path"])
    bad = 1
if clips[9].get("name") != "nine":
    print("  FAIL: clip 9's name changed in the save to %r" % clips[9].get("name"))
    bad = 1
raise SystemExit(bad)
PY
[ "$ok" = "1" ] && echo "  saved: the rename and the retarget are both in the project file"

# `wait` after the kill, or the shell prints its own "Terminated" job notice AFTER the verdict
# line — which reads like the check crashed at the end of a successful run.
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ "$ok" = "1" ] && echo "clip_text_check: PASS — a clip's name and source path are settable, and the edit is seen, heard and saved" \
                || { echo "clip_text_check: FAIL"; exit 1; }
