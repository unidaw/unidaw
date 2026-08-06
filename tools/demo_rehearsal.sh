#!/usr/bin/env bash
# REHEARSE THE DEMO BY PROMPTING IT, THE WAY IT WILL BE DRIVEN ON THE DAY.
#
# NOT REGISTERED IN ctest, on purpose: it makes a handful of real API calls, and a suite that
# costs money every time somebody runs it will be disabled by whoever is paying. The path itself
# is covered on every run by tools/ask_path_check.sh, which is deliberately one small call. This
# is the wider rehearsal you run before a demo, by hand:
#
#   ./tools/demo_rehearsal.sh
#
# WHAT IT CAN AND CANNOT TELL YOU. It drives the sidecar's command socket — the same socket the
# browser drives — so it proves each step is ACHIEVABLE BY PROMPTING against a real engine. It
# does NOT open a browser, so it says nothing about whether the page renders the agent log, the
# piano roll or the harmony lane. That leg still needs a human at the screen.
#
# Each step asserts the ENGINE CHANGED, never that the model sounded confident. A stubbed
# add_track that sent nothing still produced "Done! I've added a new audio track named Bass
# (track 2)" — the prose is not evidence.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
SIDE="$ROOT/ui/target/release/daw-sidecar"
[ -x "$SIDE" ] || SIDE="$ROOT/ui/target/debug/daw-sidecar"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$SIDE" ] || { echo "build daw-sidecar first"; exit 2; }
node -e "require.resolve('ws')" >/dev/null 2>&1 || { echo "needs node's 'ws' module"; exit 2; }

SIDECAR_CWD=/Users/jak/src/daw-web/ui
[ -d "$SIDECAR_CWD" ] || SIDECAR_CWD="$ROOT/ui"

TMP="$(mktemp -d)"
SHM="/demoreh_$$"
ENG=""; SC=""
cleanup() {
  [ -n "$SC" ]  && kill "$SC"  2>/dev/null
  [ -n "$ENG" ] && stop_engine "$ENG"
  echo "  logs kept in $TMP"
}
trap cleanup EXIT

read -r STATE_PORT CMD_PORT <<EOF
$(python3 -c "
import socket
def p():
    s=socket.socket(); s.bind(('127.0.0.1',0)); n=s.getsockname()[1]; s.close(); return n
print(p(), p())")
EOF

# The engine's project dir is the TEMP dir, not presets/: the harmony step verifies itself by
# saving and reading the file back, and a rehearsal must not write into the repo's presets.
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 900 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120 "UI: command thread started" \
  || { echo "engine never came up"; tail -8 "$TMP/eng.log"; exit 1; }

( cd "$SIDECAR_CWD" && exec env DAW_ENV_FILE="${DAW_ENV_FILE:-}" \
    "$SIDE" --shm "$SHM" --port "$STATE_PORT" --cmd-port "$CMD_PORT" >"$TMP/side.log" 2>&1 ) &
SC=$!
port_open() { nc -z 127.0.0.1 "$CMD_PORT" 2>/dev/null; }
wait_until 60 port_open || { echo "sidecar never opened its command port"; tail -8 "$TMP/side.log"; exit 1; }

cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" 2>/dev/null; }

# One prompt, one connection, printed as it streams. Returns non-zero if the model reports failure.
ask() {
  node - "$CMD_PORT" "$1" <<'JS'
const WebSocket = require('ws');
const [,, port, prompt] = process.argv;
const ws = new WebSocket(`ws://127.0.0.1:${port}`);
const deadline = setTimeout(() => { console.log('    TIMEOUT'); process.exit(3); }, 180000);
ws.on('open', () => ws.send(JSON.stringify({ type: 'ask', text: prompt })));
ws.on('message', (m) => {
  let j = null; try { j = JSON.parse(m.toString()); } catch (_) { return; }
  const t = j.agent, text = (j.text || '').replace(/\s+/g, ' ');
  if (t === 'did')  console.log('    did: ' + text.slice(0, 90) + '  ok=' + j.ok);
  // The model's PROSE, because when it declines to act the reason is only ever in here. Without
  // it a step that reports "the song did not change" cannot be told apart from one where the
  // model reasonably refused.
  else if (t === 'say') console.log('    say: ' + text.slice(0, 150));
  else if (t === 'done')   { clearTimeout(deadline); process.exit(0); }
  else if (t === 'failed') { console.log('    FAILED: ' + text.slice(0,200)); clearTimeout(deadline); process.exit(1); }
});
ws.on('error', (e) => { console.log('    WS ERROR ' + e.message); process.exit(4); });
JS
}

PASS=0; FAIL=0
step() {  # step "<label>" "<prompt>" "<measure cmd>"  — passes when the measurement CHANGES
  local label="$1" prompt="$2" measure="$3"
  local before after
  before=$(eval "$measure")
  echo "  [$label]"
  ask "$prompt"
  local rc=$?
  after=$(eval "$measure")
  if [ "$rc" -ne 0 ]; then
    echo "    RESULT: FAIL — the ask itself did not complete"; FAIL=$((FAIL+1)); return
  fi
  if [ "$before" = "$after" ]; then
    echo "    RESULT: FAIL — the model answered but the song did not change ($measure: $before)"
    FAIL=$((FAIL+1))
  else
    echo "    RESULT: pass ($before -> $after)"; PASS=$((PASS+1))
  fi
}

# Parse the published fields, never line counts — `grep -c .` on `get tracks` counts JSON lines
# and calls them tracks, which is a wrong number in a failure message.
n_tracks() {
  cli get tracks | sed -n 's/.*"track_count"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1
}
# Each track publishes its head-of-chain device NAME. A sampler landing on a track is therefore
# visible as that name appearing, which is what "put a sampler on it" has to mean to be real.
# A track's published `device` field is the HOSTED PLUGIN's name and stays empty for a sampler,
# so counting it there reports zero however well the add worked. `get sampler-kit` answers
# {"found": false} / {"found": true}, which is the actual question.
n_samplers(){
  local n=0 id
  for id in $(cli get tracks | sed -n 's/.*"track_id": *\([0-9][0-9]*\).*/\1/p'); do
    [ "$id" -gt 100000 ] && continue
    cli get sampler-kit --track "$id" 2>/dev/null | grep -q '"found": *true' && n=$((n+1))
  done
  echo "$n"
}
# EVERY track's notes, not track 0's. The model routinely makes its own track for a part, and a
# count that only ever looks at track 0 reports "the song did not change" while the notes are
# sitting one track over.
# `get patcher` publishes ONE REGION's graph — it prints `region_device` — so it is a
# UI-scoped view, not a census. It read 3 nodes before and after adding a patcher, which
# says nothing about whether the add landed. The device chain is only observable whole
# through a SAVE, same as the harmony timeline.
n_patcher(){
  cli do save rehearsal --force >/dev/null 2>&1
  local f="$TMP/rehearsal.uniproj.json"
  [ -f "$f" ] || { echo 0; return; }
  python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))
print(sum(1 for t in d['tracks'] for x in t.get('device_chain',[])
          if str(x.get('kind','')).startswith('patcher')))" "$f" 2>/dev/null || echo 0
}

# CHORDS ARE NOT NOTES and are not counted by n_notes — they live in their own array on the clip,
# because a chord is a DEGREE of the current key and stays one until it sounds. So a progression
# written by the model is invisible to the note count, and a rehearsal that only counted notes
# would report "nothing happened" for a step that worked.
n_chords(){
  cli do save rehearsal --force >/dev/null 2>&1
  local f="$TMP/rehearsal.uniproj.json"
  [ -f "$f" ] || { echo 0; return; }
  python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))
# THE CLIP IS A CHORD'S HOME. The array on a PLACEMENT is not a second home — it is that
# placement's local-edit overlay (`placement.adds`, with `mutes` alongside it for removals), the
# per-appearance edit scope. It serialises through the same writeEvents as a clip body, which is
# why it carries the same field name and why counting one and not the other is easy to do by
# accident. A locally-edited appearance would report 0 chords, which the day before a demo reads
# as "it is broken".
print(sum(len(c.get('chords',[])) for c in d.get('clips',[]))
    + sum(len(pl.get('chords',[])) for t in d.get('tracks',[]) for pl in t.get('placements',[])))" "$f" 2>/dev/null || echo 0
}

n_notes()  {
  local total=0 id
  for id in $(cli get tracks | sed -n 's/.*"track_id": *\([0-9][0-9]*\).*/\1/p'); do
    [ "$id" -gt 100000 ] && continue      # the master's stable id, not a real lane
    total=$(( total + $(cli get notes --track "$id" 2>/dev/null | grep -c '"pitch"') ))
  done
  echo "$total"
}
# For the steps whose result is a SHAPE rather than a count, hash what the engine publishes and
# require it to change. Weaker than asserting the exact value, and deliberately so: the point of a
# rehearsal is "did asking for this move the song", and pinning C-minor-specifically would fail
# the day the model reasonably picks a different voicing for the same request.
# THERE IS NO `get harmony` QUERY — I wrote one and it hashed the empty string on every call, so
# the step could only ever report "unchanged" and blame the product. The harmony timeline is only
# observable through a SAVE, so save and read the file. Valid `get` queries are transport, tracks,
# diffs, patcher, notes, meters, audio-sources, automation, extents, arrangement, clip,
# device-params, sampler-envelope, automation-points, sampler-kit, waveform.
h_harmony(){
  cli do save rehearsal --force >/dev/null 2>&1
  local f="$TMP/rehearsal.uniproj.json"
  [ -f "$f" ] || { echo "nofile"; return; }
  python3 -c "
import json,sys
try: print(len(json.load(open(sys.argv[1])).get('harmony_timeline',[])))
except Exception: print('unreadable')" "$f"
}
h_quantize(){ cli get tracks 2>/dev/null | grep -o '"quantize_grid": *[0-9]*' | tr -d '\n ' ; }

echo "REHEARSING — each step is one prompt, judged by whether the ENGINE changed."
echo
step "add a track"        "Add a new track named Bass."                     n_tracks
step "put a sampler on it" "Put a sampler on the track named Bass."         n_samplers
step "write a bassline"   "Write a simple four-bar bassline on the track named Bass, root notes on the beat." n_notes
step "the harmony lane"   "Set the key to C minor from the start of the song."   h_harmony
step "lane quantize"      "Quantize the Bass track to a 1/16 grid at full strength." h_quantize
step "a drum beat"        "Add a track called Drums with a sampler on it, and write a four-bar drum beat: kick on every beat, snare on 2 and 4." n_notes
step "a chord progression" "On a new track called Keys, write a four-bar I-V-vi-IV chord progression, strummed." n_chords
step "the patcher"        "Put a patcher device on the Bass track."                 n_patcher

echo
echo "rehearsed: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
