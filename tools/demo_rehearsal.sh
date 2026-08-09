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
if [ -L "${BASH_SOURCE[0]}" ]; then
  printf '%s\n' 'demo_rehearsal: ERROR: refusing a symlinked entrypoint' >&2
  exit 2
fi
SCRIPT_DIR="$({ CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P; })" || exit 2
. "$SCRIPT_DIR/lib/repository_root.sh" || exit 2
ROOT="$(daw_repository_root)" || exit 2
unset BASH_ENV ENV NODE_OPTIONS NODE_PATH
unset PYTHONHOME PYTHONPATH
unset DEMO_BUILD_DIR_INPUT DEMO_ENV_FILE_INPUT DEMO_API_KEY_INPUT
unset SIDECAR_API_KEY SIDECAR_ENV_FILE
DEMO_BUILD_DIR_INPUT="${DAW_BUILD_DIR:-}"
DEMO_ENV_FILE_INPUT="${DAW_ENV_FILE:-}"
DEMO_API_KEY_INPUT="${ANTHROPIC_API_KEY:-}"
while IFS= read -r daw_variable; do
  unset "$daw_variable"
done < <(compgen -v DAW_)
unset ANTHROPIC_API_KEY
export -n DEMO_BUILD_DIR_INPUT DEMO_ENV_FILE_INPUT DEMO_API_KEY_INPUT 2>/dev/null || true
command -v python3 >/dev/null 2>&1 || { echo "needs python3"; exit 2; }
python3() { command python3 -I "$@"; }
. "$ROOT/tools/lib/engine_wait.sh"
if [ -n "$DEMO_BUILD_DIR_INPUT" ]; then
  BUILD="$(daw_canonical_directory "$DEMO_BUILD_DIR_INPUT" 'explicit DAW_BUILD_DIR')" || exit 2
  printf 'build directory: %s (explicit DAW_BUILD_DIR)\n' "$BUILD" >&2
else
  BUILD="$(daw_canonical_directory "$ROOT/build" 'checkout build directory')" || exit 2
  daw_require_within_root "$BUILD" "$ROOT" 'checkout build directory' || exit 2
  daw_validate_cmake_build_source "$BUILD" "$ROOT" 'checkout build directory' || exit 2
  printf 'build directory: %s (checkout default)\n' "$BUILD" >&2
fi
SIDE="$ROOT/ui/target/release/daw-sidecar"
[ -x "$SIDE" ] || SIDE="$ROOT/ui/target/debug/daw-sidecar"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

ENGINE="$(daw_canonical_executable "$BUILD/daw_engine" 'selected daw_engine')" || { echo "build daw_engine first"; exit 2; }
HOST="$(daw_canonical_executable "$BUILD/juce_host_process" 'selected juce_host_process')" || { echo "build juce_host_process first"; exit 2; }
daw_require_within_root "$ENGINE" "$BUILD" 'selected daw_engine' || exit 2
daw_require_within_root "$HOST" "$BUILD" 'selected juce_host_process' || exit 2
SIDE="$(daw_canonical_executable "$SIDE" 'checkout daw-sidecar')" || { echo "build daw-sidecar first"; exit 2; }
CLI="$(daw_canonical_executable "$CLI" 'checkout daw-cli')" || { echo "build daw-cli first"; exit 2; }
daw_require_within_root "$SIDE" "$ROOT" 'checkout daw-sidecar' || exit 2
daw_require_within_root "$CLI" "$ROOT" 'checkout daw-cli' || exit 2
PLUGIN_CACHE="$BUILD/plugin_cache.json"
if [ -L "$PLUGIN_CACHE" ]; then
  echo "selected plugin cache is a symlink; refusing ambiguous artifact provenance"
  exit 2
fi
if [ -e "$PLUGIN_CACHE" ]; then
  PLUGIN_CACHE="$(daw_canonical_readable_file "$PLUGIN_CACHE" 'selected plugin cache')" || exit 2
  daw_require_within_root "$PLUGIN_CACHE" "$BUILD" 'selected plugin cache' || exit 2
fi
PATCHER_PRESETS="$(daw_canonical_directory "$ROOT/presets/patcher" 'checkout patcher presets')" || exit 2
daw_require_within_root "$PATCHER_PRESETS" "$ROOT" 'checkout patcher presets' || exit 2

# THE SIDECAR CARRIES THE TOOL MANIFEST, SO A STALE ONE REHEARSES YESTERDAY'S CAPABILITIES.
#
# The model can only ask for tools the sidecar advertises, and the sidecar advertises whatever was
# compiled into it. Rehearse against a binary older than daw-agent's source and the run is a
# faithful test of a build nobody is going to demo — while looking exactly like a real result.
#
# This is not hypothetical. `add_chords` landed at 11:42; the release binary was from 00:44; this
# script prefers release. The model, offered no chord tool, hand-rolled a progression out of
# add_notes and the chord step failed. Twenty minutes went into reading that as a model failure.
#
# REFUSED, not warned. A warning above a hundred lines of streaming model output is a warning
# nobody sees, and the whole value of a rehearsal is that its result can be trusted.
newest_src=$(find "$ROOT/ui/daw-agent/src" "$ROOT/ui/daw-sidecar/src" -name '*.rs' -newer "$SIDE" 2>/dev/null | head -3)
if [ -n "$newest_src" ]; then
  echo "REFUSING TO REHEARSE: $SIDE is older than the agent/sidecar source."
  echo "  newer than the binary:"
  printf '%s\n' "$newest_src" | sed 's/^/    /'
  echo "  The sidecar compiles in the TOOL MANIFEST, so the model would be offered a stale set of"
  echo "  tools and the run would test a build you are not going to demo. Rebuild first:"
  echo "      ( cd $ROOT/ui && cargo build --release -p daw-sidecar )"
  exit 2
fi
command -v node >/dev/null 2>&1 || { echo "needs node"; exit 2; }
NODE_MODULES="$ROOT/ui-web/node_modules"
[ -d "$NODE_MODULES" ] && [ ! -L "$NODE_MODULES" ] \
  || { echo "needs checkout-local ui-web dependencies as a real directory"; exit 2; }
NODE_MODULES="$(daw_canonical_directory "$NODE_MODULES" 'ui-web dependency directory')" || exit 2
daw_require_within_root "$NODE_MODULES" "$ROOT" 'ui-web dependency directory' || exit 2
WS_ENTRY="$(cd "$ROOT/ui-web" && node -e "process.stdout.write(require('fs').realpathSync(require.resolve('ws')))" 2>/dev/null)" \
  || { echo "needs checkout-local node 'ws' module"; exit 2; }
case "$WS_ENTRY" in
  "$NODE_MODULES"/*) ;;
  *) echo "node 'ws' resolved outside checkout-local dependencies"; exit 2 ;;
esac

# This is an intentionally paid rehearsal. Credentials must be named by the
# caller; the run-owned sidecar cwd below prevents implicit checkout/home .env
# discovery from broadening that authority.
SIDECAR_ENV_FILE=''
if [ -n "$DEMO_ENV_FILE_INPUT" ]; then
  SIDECAR_ENV_FILE="$(daw_canonical_readable_file "$DEMO_ENV_FILE_INPUT" 'explicit DAW_ENV_FILE')" || exit 2
fi
case "$DEMO_API_KEY_INPUT" in
  *[![:space:]]*) ;;
  *) DEMO_API_KEY_INPUT='' ;;
esac
if [ -z "$DEMO_API_KEY_INPUT" ]; then
  if [ -z "$SIDECAR_ENV_FILE" ] || ! daw_env_file_has_anthropic_key "$SIDECAR_ENV_FILE"; then
    echo "demo rehearsal requires explicit ANTHROPIC_API_KEY or DAW_ENV_FILE"
    exit 2
  fi
fi
SIDECAR_API_KEY="$DEMO_API_KEY_INPUT"
DEMO_API_KEY_INPUT=''
export -n SIDECAR_API_KEY SIDECAR_ENV_FILE 2>/dev/null || true

TMP="$(daw_make_temp_directory daw-demo-rehearsal)" || exit 2
SIDECAR_CWD="$TMP/runtime/sidecar/cwd"
mkdir -p -- "$SIDECAR_CWD" || exit 2
SIDECAR_CWD="$(daw_canonical_directory "$SIDECAR_CWD" 'run-owned sidecar cwd')" || exit 2
daw_require_within_root "$SIDECAR_CWD" "$TMP" 'run-owned sidecar cwd' || exit 2
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
    DAW_HOST_BINARY="$HOST" DAW_PLUGIN_CACHE="$PLUGIN_CACHE" \
    DAW_PATCHER_PRESET_DIR="$PATCHER_PRESETS" \
    "$ENGINE" --run-seconds 900 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120 "UI: command thread started" \
  || { echo "engine never came up"; tail -8 "$TMP/eng.log"; exit 1; }

( cd "$SIDECAR_CWD" && exec env ANTHROPIC_API_KEY="$SIDECAR_API_KEY" \
    DAW_ENV_FILE="$SIDECAR_ENV_FILE" DAW_PROJECT_DIR="$TMP" \
    "$SIDE" --shm "$SHM" --port "$STATE_PORT" --cmd-port "$CMD_PORT" \
      --plugin-cache "$PLUGIN_CACHE" >"$TMP/side.log" 2>&1 ) &
SC=$!
port_open() { nc -z 127.0.0.1 "$CMD_PORT" 2>/dev/null; }
wait_until 60 port_open || { echo "sidecar never opened its command port"; tail -8 "$TMP/side.log"; exit 1; }

cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" 2>/dev/null; }

# One prompt, one connection, printed as it streams. Returns non-zero if the model reports failure.
ask() {
  ( cd "$ROOT/ui-web" && node - "$CMD_PORT" "$1" ) <<'JS'
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
# THE KIT READ-BACK IS A REQUEST, NOT A FIELD. `get sampler-kit` writes a request into the ring
# and reads the answer the engine publishes in reply, so asking the instant after add_device can
# legitimately answer found:false — the device is there and the answer is not yet. This counted
# once and reported 0 for a track whose chain held a sampler, failing the step and blaming the
# model for a round trip nobody waited for.
#
# Retried rather than slept: the answer usually arrives immediately, and a fixed sleep would pay
# the worst case on every call.
n_samplers(){
  local n=0 id try
  for id in $(cli get tracks | sed -n 's/.*"track_id": *\([0-9][0-9]*\).*/\1/p'); do
    [ "$id" -gt 100000 ] && continue
    for try in 1 2 3 4 5 6 7 8; do
      if cli get sampler-kit --track "$id" 2>/dev/null | grep -q '"found": *true'; then
        n=$((n+1)); break
      fi
      sleep 0.25
    done
  done
  echo "$n"
}
# A SAMPLER THAT CAN ACTUALLY SOUND, which is not the same fact as a sampler being present and is
# the one that decides whether the demo makes noise. n_samplers above counts DEVICES: it answered
# "yes, a sampler" for a bank with nothing in it, so the AI could add an instrument, write sixteen
# notes and pass every step of this rehearsal with a track that is audibly nothing. That is what a
# count-based check cannot see, and it went unnoticed because no step here ever listened.
#
# `length_frames` is the engine's own verdict: 0 means the slot's source did not resolve, so the
# slot exists, draws, and is silent. Counting SLOTS would repeat the original mistake one level
# down.
n_sounding() {
  local n=0 id
  for id in $(cli get tracks | sed -n 's/.*"track_id": *\([0-9][0-9]*\).*/\1/p'); do
    [ "$id" -gt 100000 ] && continue
    cli get sampler-kit --track "$id" 2>/dev/null \
      | grep -oE '"length_frames": *[0-9]+' | grep -qvE ': *0$' && n=$((n+1))
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
# COUNTS ONLY THE PATCHERS THAT COULD ACTUALLY SOUND — an EVENT patcher after the instrument is
# not a patcher that works, it is a silent track with a valid-looking chain.
#
# This counted every patcher device regardless of position, and so reported success right through
# a regression that put the event patcher AFTER the sampler: add_device's default was changed from
# head-insert to append on 2026-08-06 to match the other surfaces, which is correct for an effect
# and wrong for a generator. The device was present, the graph was valid, the count went 0 -> 1,
# and the graph emitted into nothing. A step that cannot tell those apart is not testing the claim
# it is named after.
#
# Returning 0 rather than a differently-worded answer is deliberate: `step` passes when the value
# CHANGES, so encoding the mistake in the string would still read as a pass.
n_patcher(){
  cli do save rehearsal --force >/dev/null 2>&1
  local f="$TMP/rehearsal.uniproj.json"
  [ -f "$f" ] || { echo 0; return; }
  python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))
n=0
for t in d['tracks']:
    chain=[str(x.get('kind','')) for x in t.get('device_chain',[])]
    inst=[i for i,k in enumerate(chain) if k in ('sampler','vst_instrument')]
    first_inst = min(inst) if inst else len(chain)
    for i,k in enumerate(chain):
        if not k.startswith('patcher'):
            continue
        # An EVENT patcher only counts ahead of the instrument it feeds. The audio and instrument
        # flavours process what reaches them, so their position is not this rule's business.
        if k == 'patcher_event' and i > first_inst:
            continue
        n+=1
print(n)" "$f" 2>/dev/null || echo 0
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
# THE CLIP IS A CHORD'S HOME. The array on a PLACEMENT is not a second home - it is that
# placement's local-edit overlay (placement.adds, with mutes alongside it for removals), the
# per-appearance edit scope. It serialises through the same writeEvents as a clip body, which is
# why it carries the same field name and why counting one and not the other is easy to do by
# accident. A locally-edited appearance would report 0 chords, which the day before a demo reads
# as \"it is broken\".
#
# NO BACKTICKS IN HERE. This block sits inside python3 -c \"...\", a DOUBLE-QUOTED shell string,
# so a backtick opens command substitution: the first version of this comment quoted the field
# names -- in BACKTICKS, inside this same double-quoted string -- and the shell duly tried to run
# them, printing 'command not found' twice per call. The comment WARNING about backticks was
# written using backticks, so it re-created the bug it documents, on every single call, and
# survived the fix that was supposed to remove it. Prose about code is still code to the shell.
# Harmless only by luck - the substitutions came back empty and left the comment
# a comment.
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

# SOMETHING TO LOAD. The engine runs with DAW_PROJECT_DIR="$TMP", and a bare sample name
# resolves there, so a wav written here is exactly what the demo's "load a kick into it" reaches.
# Generated rather than copied from presets/audio: the two files there are the WAVEFORM PROBE
# assets, which are silent for their first second and stretch that when played below their root —
# a rehearsal that loaded one and heard nothing would be reporting the probe's shape, not the
# product's.
python3 - "$TMP/demo_kick.wav" <<'PYWAV'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
# Attack in the first millisecond, so it sounds the instant a note starts at any transposition.
frames = [int(32000 * math.exp(-i / 3000.0) * math.sin(i * 0.08)) for i in range(8000)]
w.writeframes(b''.join(struct.pack('<h', f) for f in frames))
w.close()
PYWAV

echo "REHEARSING — each step is one prompt, judged by whether the ENGINE changed."
echo
step "add a track"        "Add a new track named Bass."                     n_tracks
step "put a sampler on it" "Put a sampler on the track named Bass."         n_samplers
step "load a sample"      "Load demo_kick.wav into the sampler on the track named Bass." n_sounding
step "write a bassline"   "Write a simple four-bar bassline on the track named Bass, root notes on the beat." n_notes
step "the harmony lane"   "Set the key to C minor from the start of the song."   h_harmony
step "lane quantize"      "Quantize the Bass track to a 1/16 grid at full strength." h_quantize
# MEASURED BY WHETHER IT CAN SOUND, not by whether notes appeared. This step passed for weeks
# while producing a SILENT track: the model adds the sampler, writes sixteen notes, and n_notes
# duly reports 16 -> 32. Today it also tried load_sample twice, got ok=false both times because it
# GUESSED at file names, said "you'll need to load your own drum samples later", wrote the notes
# anyway — and still scored a pass. A rehearsal that scores the demo's centrepiece on note count
# is agreeing with the model's own workaround.
#
# THIS STEP IS EXPECTED TO FAIL UNTIL THE AGENT CAN DISCOVER WHAT SAMPLES EXIST. That is the point
# of putting it here: the failure is true, and the step that lies is worse than the step that
# fails. The prompt deliberately does NOT name a file, because Jaakko will not name one either.
step "a drum beat"        "Add a track called Drums with a sampler on it, and write a four-bar drum beat: kick on every beat, snare on 2 and 4." n_sounding
step "a chord progression" "On a new track called Keys, write a four-bar I-V-vi-IV chord progression, strummed." n_chords
step "the patcher"        "Put a patcher device on the Bass track."                 n_patcher

# ---- AND DOES THE SONG ACTUALLY MAKE A NOISE.
#
# ASKED OF THE WHOLE SONG AT THE END, not step by step, because the per-step version kept missing.
# Each step measures the thing its own prompt was about — notes for a bassline, chords for a
# progression, a device for the patcher — and every one of those can be perfectly correct on a
# track that emits nothing. The drum step scored 16 -> 32 for a silent track; the chord step
# scored 0 -> 4 for a track with NO DEVICES AT ALL. Two of the seven things being demonstrated,
# both green, both silent.
#
# A per-step audio assertion would have to be written into each new step and would be forgotten in
# exactly the same way. This is one rule over the finished song: anything carrying musical content
# must have something that can sound it. It applies to steps nobody has written yet.
#
# STRUCTURAL, and deliberately weaker than a render. A render says "this song made noise", which a
# single loud track satisfies while three others are silent; per-track audio needs stems and a solo
# pass and is not a night-before change. "Has an instrument that could sound this" is the property
# that was actually violated, and it names the track.
echo
cli do save rehearsal --force >/dev/null 2>&1
# THE FILE SAYS WHAT IS ON EACH TRACK; THE ENGINE SAYS WHETHER IT CAN SOUND. Splitting it that
# way is not tidiness — the first version of this check read `slots` out of the saved file and
# passed the Drums track, which held TWO slots pointing at kick.wav and snare.wav, neither of
# which exists. A refused load still MINTS A SLOT, so slot COUNT is exactly the wrong question,
# and answering it from the file would mean re-implementing the engine's path resolution here as
# a second copy that guesses at the same directories.
#
# `length_frames` is the engine's own verdict and it is only available live.
ROWS="$(python3 - "$TMP/rehearsal.uniproj.json" 2>/dev/null <<'PYQ'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    raise SystemExit
clips = {c.get("id"): c for c in d.get("clips", [])}
for t in d.get("tracks", []):
    if t.get("is_master"):
        continue
    # BOTH PROJECT FORMATS. schema_version 4 keeps notes in CLIPS that placements point at;
    # schema_version 1 keeps them FLAT on the track. Counting only clips is not a partial answer,
    # it is a zero for every schema-1 track — so this phase would report "every track that carries
    # notes has an instrument" about a song whose notes it never looked at, which is the precise
    # failure it exists to catch, one level up. Five of the nine shipped presets are schema 1,
    # including maximal, which has 162 notes, no clips at all, and renders at peak 0.66.
    #
    # The rehearsal happens to save schema 4 today. That is luck, not a reason.
    content = len(t.get("notes", [])) + len(t.get("chords", []))
    for pl in t.get("placements", []):
        c = clips.get(pl.get("clip_id"))
        if c:
            content += len(c.get("notes", [])) + len(c.get("chords", []))
        content += len(pl.get("chords", []))
    if content == 0:
        continue
    kinds = [x.get("kind") for x in t.get("device_chain", [])]
    # `track_id`, NOT `id`. Getting this wrong did not raise: t.get("id") returned None, the
    # query became `--track None`, and the engine answered found:false for EVERY track — so the
    # phase reported all three as silent, including the one that demonstrably sounded. A wrong
    # field name reads exactly like a real defect, which is why the id is asserted here rather
    # than formatted into a command and hoped for.
    tid = t.get("track_id")
    if tid is None:
        print("HARNESS_BUG no_track_id_field 0 0 0 %s" % ",".join(sorted(t.keys())))
        continue
    name = (t.get("name") or ("track %s" % tid)).replace(" ", "_")
    print("%s %s %d %d %d %s" % (tid, name, content,
                                 int("vst_instrument" in kinds), int("sampler" in kinds),
                                 ",".join(kinds) or "no_devices_at_all"))
PYQ
)"
SILENT=""
while read -r tid tname content has_vst has_smp kinds; do
  [ -n "${tid:-}" ] || continue
  if [ "$tid" = "HARNESS_BUG" ]; then
    SILENT="$SILENT|THIS CHECK IS BROKEN, not the song: no track_id field in the saved project (keys: $kinds)"
    continue
  fi
  [ "$has_vst" = "1" ] && continue
  if [ "$has_smp" != "1" ]; then
    SILENT="$SILENT|$tname: $content note/chord event(s) and NO INSTRUMENT ($kinds)"
    continue
  fi
  # A slot whose source did not resolve reports length_frames 0 — minted, drawn, and silent.
  #
  # RETRIED, for the reason n_samplers above is retried and this phase was not: the kit is a
  # REQUEST, not a field, so a single ask can legitimately answer found:false while the engine is
  # still filling the answer. Asking once flagged the BASS track — the one track here that
  # demonstrably sounds — as silent. Two hazards in one line: an answer that has not arrived reads
  # exactly like an answer of "nothing", and </dev/null keeps the real `cli` from eating the rows
  # this loop is still reading from its stdin.
  resolved=0; kit=""
  for _try in 1 2 3 4 5 6 7 8; do
    kit="$(cli get sampler-kit --track "$tid" </dev/null 2>/dev/null)"
    if printf '%s' "$kit" | grep -oE '"length_frames": *[0-9]+' | grep -qvE ': *0$'; then
      resolved=1; break
    fi
    sleep 0.25
  done
  if [ "$resolved" = "0" ]; then
    # THE FAILURE CARRIES THE ANSWER IT JUDGED. Without this the message says "no slot resolves"
    # for three indistinguishable causes — the kit says found:false, the kit has slots that are
    # all zero-length, or the query never reached the right track at all — and the first time this
    # phase fired it flagged a track that demonstrably sounded, which is a bug in the CHECK that
    # a verdict-only message cannot tell apart from a bug in the product.
    why="$(printf '%s' "$kit" | grep -oE '"found": *(true|false)|"length_frames": *[0-9]+' \
           | head -4 | tr '\n' ' ')"
    [ -n "$why" ] || why="the kit query returned nothing at all for track $tid"
    SILENT="$SILENT|$tname (track $tid): $content note/chord event(s), sampler present, no slot resolves its source — kit said: $why"
  fi
done <<< "$ROWS"

if [ -n "$SILENT" ]; then
  echo "  SILENT TRACKS — content written where nothing can sound it:"
  # QUOTED AND SPLIT ON THE SEPARATOR ONLY. Unquoted, the shell word-splits each message on every
  # space and printf emits one line PER WORD — "Drums:", "17", "note/chord" — which is unreadable
  # exactly when somebody is reading it to find out what broke.
  printf '%s\n' "$SILENT" | tr '|' '\n' | while IFS= read -r line; do
    [ -n "$line" ] && echo "    $line"
  done
  FAIL=$((FAIL+1))
else
  echo "  every track carrying notes or chords has something that can sound it"
fi

# ---- AND THE ENGINE ACTUALLY PRODUCES AUDIO FROM WHAT THE AI BUILT.
#
# The phase above asks whether each part COULD sound — a structural question, answered from the
# chain and the kit. This one closes the loop: the song the model built by prompting is rendered
# OFFLINE and the result is measured. Prompt in, audio out, with nothing in between that a
# count could satisfy.
#
# THE OFFLINE RENDER RATHER THAN PLAYBACK, deliberately: no audio device, no dropouts, no
# dependence on what this machine's default output is doing. The capture tap on this Mac is
# unusable and a live playback assertion would be measuring CoreAudio.
#
# ITS LIMITATION, stated because it is easy to over-read: a whole-song peak is satisfied by ONE
# loud track. It cannot tell you the drums sounded — that is what the per-track phase above is
# for, and the two are complementary rather than redundant. Per-track audio would need stems and
# a solo pass per track.
echo
if [ -f "$TMP/rehearsal.uniproj.json" ]; then
  # ITS OWN SEGMENT NAME. The rehearsal's engine is still alive on $SHM at this point, and an
  # engine with no DAW_UI_SHM_NAME takes the DEFAULT one — from which the host socket paths and
  # the shared segment are derived. Two engines on one segment is the collision that started the
  # instance-isolation work in this repo; leaving it to chance because "nothing else is probably
  # using the default right now" is how it happened the first time. Derived from $SHM so it is
  # unique per run and obviously related to it.
  ( cd "$BUILD" && exec env DAW_UI_SHM_NAME="${SHM}_render" DAW_PROJECT_DIR="$TMP" \
      DAW_HOST_BINARY="$HOST" DAW_PLUGIN_CACHE="$PLUGIN_CACHE" \
      DAW_PATCHER_PRESET_DIR="$PATCHER_PRESETS" \
      "$ENGINE" --project rehearsal \
      --render "$TMP/take" --run-seconds 12 ) >"$TMP/render.log" 2>&1
  PEAK="$(python3 - "$TMP/take.wav" <<'PYR' 2>/dev/null
import wave, struct, sys
try:
    w = wave.open(sys.argv[1], "rb")
except Exception:
    print(-1); raise SystemExit
n = w.getnframes()
d = w.readframes(min(n, 44100 * 10))
s = struct.unpack("<%dh" % (len(d) // 2), d)
print(max((abs(v) for v in s), default=0))
PYR
)"
  if [ "${PEAK:--1}" = "-1" ]; then
    echo "  the render wrote no readable wav — see $TMP/render.log"
    FAIL=$((FAIL+1))
  elif [ "${PEAK:-0}" -gt 300 ]; then
    echo "  the song the model built RENDERS AUDIO: peak $PEAK"
  else
    echo "  THE WHOLE SONG RENDERS SILENT: peak $PEAK. Every part above can be structurally
        perfect and this still be zero — that is the point of measuring it."
    FAIL=$((FAIL+1))
  fi
else
  echo "  no saved project to render, so the audio end of this was not tested at all"
  FAIL=$((FAIL+1))
fi

echo
echo "rehearsed: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
