#!/usr/bin/env bash
# EVERY OPERATION MUST BE UNDOABLE. This counts how far that is from true.
#
# Owner's ruling, 2026-08-06: "every operation must be undoable", "this is a big core issue that
# must be solved correctly", and redo must be the exact opposite of undo. An audit the same day
# put the gap at 55 of 70 mutating commands. A number that large is not a bug list, it is an
# architecture defect — and the thing that let it grow was that NOTHING MEASURED IT. Undo's
# coverage was a property of a data structure's shape (TrackStoreState is {placements, clips,
# editable}) that no test ever asked about.
#
# So this check exists before the fix does. It turns "undo is incomplete" from a sentence into a
# number that can only go up, and it is correct under every architecture the design panel
# considered — snapshots, diffs, persistent values — because it tests BEHAVIOUR THROUGH THE
# COMMAND WIRE and knows nothing about the mechanism.
#
# THREE ASSERTIONS PER COMMAND, and the first is the one usually missing:
#
#   1. THE COMMAND CHANGED THE DOCUMENT. Without this a silent no-op passes perfectly: nothing
#      changed, so undo "restored" it. This repo has shipped that exact shape more than once.
#   2. UNDO RESTORES IT BYTE-IDENTICALLY, compared through the canonical serializer rather than
#      field by field — a hand-listed comparison goes stale the day a field is added, which is
#      how undo got here.
#   3. REDO RE-REACHES the post-state. Redo is not a separate feature to test later: an undo you
#      cannot redo is a different bug wearing the same clothes.
#
# EVERY ENUM ENTRY MUST BE CLASSIFIED. The classification table below is compared against
# UiCommandType in apps/event_payloads.h, and an unclassified opcode FAILS THE CHECK. That is the
# shell equivalent of the `switch` with no `default:` label that apps/device_chain.h uses to make
# -Wswitch report a new DeviceKind — a new command cannot quietly arrive with no undo story.
#
#   tools/undo_ratchet_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

# ---------------------------------------------------------------------------------------------
# THE RATCHET. The number of commands that pass all three assertions. It may only go UP.
#
# A ratchet rather than a pass/fail line because the honest state today is 'mostly broken': a
# check that is simply RED until an architecture lands is a check that gets muted, and then the
# regression it would have caught arrives unseen. This one goes red the moment coverage DROPS,
# which is the property worth having during a refactor that will move every one of these paths.
# ---------------------------------------------------------------------------------------------
# 4 of 15 exercised, 2026-08-06, before any architecture work: WriteNote, DeleteNote, WriteChord,
# WriteHarmony. The audit puts the true figure at 10 of 70 across the whole enum; this harness
# drives 15 of them so far. Both numbers move up together as commands become undoable AND as more
# of them get exercised — which is why the ratchet counts PROVEN, not claimed.
# 15 of 15 since the switchover (undo Step 2c): undo is a cursor over whole-document versions, so
# a command is undoable by construction rather than by somebody having widened a struct for it.
EXPECTED_UNDOABLE=15
# THE NAMES THAT PASS, sorted — checked alongside the count, because a COUNT IS SATISFIED BY ANY
# FIFTEEN. Break WriteNote, make some other opcode work, and the total is unchanged while undo has
# silently regressed on the most common edit in the program. Found by the review panel.
#
# The DAW_UNDO_EXPECTED override was REMOVED: this is an equality gate, so an env override is a
# way to bless a regression from the command line without anyone reading a diff. The branch that
# raised the count to 15 still carried the override; taking main's hardening means it goes.
EXPECTED_NAMES="AddDevice AddTrack DeleteNote RemoveDevice SetLaneQuantize SetTempo SetTrackAllowNoteOverlap SetTrackCollapsed SetTrackHarmonyQuantize SetTrackMixer SetTrackName SetTrackSoundAddressed WriteChord WriteHarmony WriteNote"

TMP="$(mktemp -d)"
SHM="/undoratchet_$$"
ENG=""
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
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

# ---------------------------------------------------------------------------------------------
# CLASSIFICATION. Every UiCommandType entry must appear exactly once.
#
#   skip <Name> <reason>              - changes no persisted document state. Not a defect.
#   mutate <Name> <setup|-> <cmd...>  - changes the document; must survive undo and redo.
#
# `skip` reasons are checked by a human, not by the machine: the machine only enforces that a
# decision was RECORDED. An opcode nobody has thought about is the failure this table prevents.
# ---------------------------------------------------------------------------------------------
CLASSIFIED=""
SKIPPED=0
declare -a MUT_NAME MUT_SETUP MUT_CMD

skip()   { CLASSIFIED="$CLASSIFIED $1"; SKIPPED=$((SKIPPED + 1)); }
mutate() {
  CLASSIFIED="$CLASSIFIED $1"
  MUT_NAME+=("$1"); MUT_SETUP+=("$2"); MUT_CMD+=("$3")
}

# --- not document state -----------------------------------------------------------------------
skip None                  "not a command"
skip Undo                  "undo itself"
skip Redo                  "redo itself"
skip TogglePlay            "transport, never persisted"
skip Stop                  "transport"
skip SetPosition           "transport"
skip Panic                 "transport safety"
skip Quit                  "process lifecycle"
skip PreviewNote           "audition, writes nothing"
skip SaveProject           "writes the document out; changes nothing in it"
skip LoadProject           "replaces the document wholesale — undo across a load is a separate ruling"
skip SaveModule            "writes a file"
skip LoadModule            "replaces state wholesale, same ruling as LoadProject"
skip SavePatcherPreset     "writes a file"
skip OpenPluginEditor      "opens a window"
skip RequestClipWindow     "query"
skip RequestChainSnapshot  "query"
skip RequestDeviceParams   "query"
skip RequestWaveform       "query"
skip RequestAutomationLane "query"
skip RequestSamplerKit     "query"
skip RequestSamplerEnvelope "query"
skip BulkChunk             "transport frame for a larger payload, not a command"
skip SetLoopRange          "transport state, re-derived at load (audit: correctly excluded)"
skip SetModSourceValue     "session-only: modSources is never serialised"

# --- document state: must be undoable ---------------------------------------------------------
# setup runs BEFORE the baseline save, so the command under test has something to act on.
mutate WriteNote      "-"                                    "do note --track 0 --nanotick 0 --pitch 60 --duration $Q --column 0"
mutate DeleteNote     "do note --track 0 --nanotick 0 --pitch 60 --duration $Q --column 0" \
                                                             "do delete-note --track 0 --nanotick 0 --pitch 60 --column 0"
mutate WriteChord     "-"                                    "do chord --track 0 --nanotick 0 --degree 1 --quality 1"
mutate WriteHarmony   "-"                                    "do harmony --nanotick 0 --root 0 --scale 2"
mutate SetTempo       "-"                                    "do set-tempo --bpm 140"
mutate AddTrack       "-"                                    "do add-track"
mutate SetTrackName   "-"                                    "do rename --track 0 --name Ratchet"
mutate SetTrackMixer  "-"                                    "do mixer --track 0 --gain-db -6"
mutate AddDevice      "-"                                    "do add-device --track 0 --kind sampler --device-id 7"
mutate RemoveDevice   "do add-device --track 0 --kind sampler --device-id 7" \
                                                             "do remove-device --track 0 --device 7"
mutate SetLaneQuantize "-"                                   "do quantize --track 0 --grid 240000 --strength 100"
mutate SetTrackAllowNoteOverlap "-"                          "do note-overlap --track 0 --on 1"
mutate SetTrackCollapsed "-"                                 "do collapse --track 0 --on 1"
mutate SetTrackSoundAddressed "-"                            "do sound-addressed --track 0 --on 1"
mutate SetTrackHarmonyQuantize "-"                           "do harmony-quantize --track 0 --on 1"

# Classified but not yet exercised. These count as NOT undoable — which is honest: an untested
# command and a broken one are the same thing to a user. Each needs a setup this harness does not
# build yet; they are listed so the enum sweep stays complete and so the work is visible.
# SetRowOps and AddMarker were EXERCISED WRONGLY at first and reported "changed NOTHING" — which
# reads as a product finding and was a harness bug: set-row-ops addresses a note by ID
# (--track --clip --note), not by tick and column, and marker takes no --nanotick. A wrong
# invocation that changes nothing is indistinguishable from a command that does nothing, so it is
# better to declare them unexercised than to leave a vacuous row that looks like evidence.
# SetRowOps needs a note id read back before it can be driven; AddMarker needs the verb's real
# signature. Both are undoable per the audit, so exercising them will RAISE the ratchet.
for pending in SetRowOps AddMarker DeleteHarmony DeleteChord SetAutomationTarget MoveDevice UpdateDevice \
               SetDeviceEuclideanConfig SetTrackRouting AddModLink RemoveModLink SetModLinkUid16 \
               SetModLinkDepth AddPatcherNode RemovePatcherNode ConnectPatcherNodes \
               SetPatcherNodeConfig LoadPluginOnTrack SetDeviceParam RemoveTrack MovePlacement \
               RemovePlacement ResizePlacement AddPlacement RevertPlacementOverrides \
               WriteAutomationPoint SetPlacementEditScope RemoveMarker RenameMarker MoveMarker \
               SetTimeSignature InsertRemoveTime ForkPlacementClip SwapPlacementClip \
               ClearPlacementAlternate SamplerLoad SamplerSetSlot SamplerSlice SamplerMarker \
               SamplerEmitRows SamplerSetEnvelope SamplerSetEnvelopePoints SamplerSetLfo \
               SamplerSetFilter SetTrackLinesPerBeat SamplerSetDevice SamplerSetSlotName \
               SamplerSetVintage SetClipGrid SetAudioClipField DeleteAutomationPoint \
               SetClipText SetMarkerColor; do
  CLASSIFIED="$CLASSIFIED $pending"
done

# ---------------------------------------------------------------------------------------------
# THE ENUM SWEEP. Unclassified opcode => FAIL. This is the no-`default:` label.
# ---------------------------------------------------------------------------------------------
# SCOPED TO THE UiCommandType BLOCK. event_payloads.h holds a dozen enums — UiDiffType, the
# sampler field selectors, the reject reasons — and a bare grep for `Name = 7,` collects all of
# them. The first run of this sweep demanded an undo story for `NoSuchSlot` and `FadeInNanoticks`,
# which is the check failing to know what a command IS. awk between the declaration and its
# closing brace is the smallest thing that cannot drift when a new enum is added to the file.
ENUM_NAMES="$(awk '/^enum class UiCommandType/,/^};/' "$ROOT/apps/event_payloads.h" \
              | grep -oE '^\s+[A-Z][A-Za-z0-9]+ = [0-9]+,' \
              | sed 's/[ ,]//g' | cut -d= -f1 | sort -u)"
[ -n "$ENUM_NAMES" ] || { echo "  FAIL: parsed no UiCommandType entries — the sweep would pass vacuously"; exit 1; }

MISSING=""
for name in $ENUM_NAMES; do
  case " $CLASSIFIED " in
    *" $name "*) : ;;
    *) MISSING="$MISSING $name" ;;
  esac
done
if [ -n "$MISSING" ]; then
  echo "  FAIL: UiCommandType entries with no undo classification:$MISSING"
  echo "        Every command must be recorded as skip (changes no document state, with a"
  echo "        reason) or mutate (must survive undo and redo). A new command with no undo"
  echo "        story is the defect this sweep exists to prevent."
  exit 1
fi
echo "  enum sweep: all $(echo "$ENUM_NAMES" | wc -w | tr -d ' ') UiCommandType entries classified ($SKIPPED skipped)"

# ---------------------------------------------------------------------------------------------
# THE ENGINE
# ---------------------------------------------------------------------------------------------
python3 - "$TMP/base.uniproj.json" <<'PY'
import json, sys
Q = 960000
tr = {"track_id": 0, "name": "T", "lines_per_beat": 4, "harmony_quantize": False,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"to_master": True}, "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "base"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(sys.argv[1], "w"))
PY

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 600 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" >/dev/null 2>&1; }
wait_for_boot "$TMP/eng.log" "$ENG" 80 'UI: command thread started'

# THE DOCUMENT AS THE SERIALIZER SEES IT. Comparing saved files rather than published state is
# the whole point: the serializer is the canonical, complete projection of the document, so a
# field nobody remembered still gets compared. A field-by-field assertion would have the same
# blind spot that produced this defect.
# ALWAYS SAVED UNDER ONE NAME, then copied away. The saved document contains meta.name, so
# saving the before/after/undone states under DIFFERENT names made every pair differ by that
# field alone — which silently inverted both directions of this check: assertion 1 ("the command
# changed the document") passed for every command including ones that changed nothing, and
# assertions 2 and 3 could never pass for any command at all. The first run reported 0 of 17
# undoable, contradicting an adversarially-verified audit that says notes ARE undoable, and the
# contradiction is what exposed it. A comparison that cannot succeed is not a strict test, it is
# a broken one.
# DELETED BEFORE IT IS CLAIMED, then waited for. The first version polled for the file to be
# non-empty — and after the first save the file ALWAYS exists, so it returned instantly with the
# PREVIOUS save's bytes. Under `ctest -j2` that read three commands as un-undoable that pass
# standalone, which is a flake that would have been blamed on the engine. Removing it first makes
# the wait mean what it says. Same rule as the golden-WAV suites: put the guard where the
# resource is claimed, not at the call sites that remember.
snap() {  # snap <label> -> path of a copy of the saved document
  local src="$TMP/probe.uniproj.json"
  rm -f "$src"
  cli do save probe --force
  for _ in $(seq 1 60); do [ -s "$src" ] && break; sleep 0.1; done
  [ -s "$src" ] || { echo "  FAIL: the engine never wrote $src — every comparison below is void"; exit 1; }
  cp "$src" "$TMP/$1.json"
  echo "$TMP/$1.json"
}

PASS=0; FAILED=0; PASSING_NAMES=""
declare -a FAIL_LINES

for i in "${!MUT_NAME[@]}"; do
  name="${MUT_NAME[$i]}"; setup="${MUT_SETUP[$i]}"; cmd="${MUT_CMD[$i]}"

  # WAITED FOR, NOT SLEPT. after_command returns when the engine has appended its history line,
  # so this cannot pass or fail on how busy the machine is — the flake above was a 0.4s guess.
  after_command "$TMP" cli do load base --force >/dev/null 2>&1 || true
  [ "$setup" != "-" ] && { after_command "$TMP" cli $setup >/dev/null 2>&1 || true; }

  before="$(snap "before_$i")"

  # SEND UNTIL THE ENGINE ACCEPTS, BOUNDED — because a REFUSAL IS NOT A VERDICT ON UNDO.
  #
  # Version-gated commands (WriteNote, WriteHarmony, the clip edits) stamp their base from the
  # PUBLISHED state, and the published version LAGS the engine's after a load — task #120. So a
  # command can be refused for reasons that have nothing to do with whether it is undoable, and
  # assertion 1 below then reports "the command changed NOTHING ... may be a silent no-op", which
  # is an accusation aimed at the wrong thing entirely. That is exactly what happened: WriteHarmony
  # came back 14-of-15 in a full-suite run and passed alone twice, and the cause was
  # harmony.version_mismatch base:9 current:10 — the engine correctly declining a stale write.
  #
  # Each send re-reads the version, so this converges as soon as the publish catches up.
  refusals() { grep -c '"outcome":"rejected' "$TMP/history.jsonl" 2>/dev/null || true; }
  accepted=0
  for _ in 1 2 3 4 5; do
    r_before="$(refusals)"
    after_command "$TMP" cli $cmd >/dev/null 2>&1 || true
    if [ "$(refusals)" -eq "$r_before" ]; then accepted=1; break; fi
    sleep 0.3
  done
  snap "after_$i" >/dev/null

  # A COMMAND THE ENGINE REFUSED IS REPORTED AS A REFUSAL, not as a no-op. The two look identical
  # from the document — both leave it unchanged — and only one of them says anything about undo.
  if [ "$accepted" -eq 0 ]; then
    FAILED=$((FAILED + 1))
    FAIL_LINES+=("$name: the engine REFUSED this command on all 5 attempts (stale base — see task #120), so undo was never exercised. This is not an undo regression.")
    continue
  fi

  # 1. IT CHANGED SOMETHING. A command that did nothing would pass 2 and 3 perfectly.
  if cmp -s "$TMP/before_$i.json" "$TMP/after_$i.json"; then
    FAILED=$((FAILED + 1))
    FAIL_LINES+=("$name: the command changed NOTHING in the document — undo is untestable and the command may be a silent no-op")
    continue
  fi

  # 2. UNDO RESTORES IT, byte for byte.
  after_command "$TMP" cli do undo >/dev/null 2>&1 || true
  snap "undone_$i" >/dev/null
  if ! cmp -s "$TMP/before_$i.json" "$TMP/undone_$i.json"; then
    FAILED=$((FAILED + 1))
    FAIL_LINES+=("$name: UNDO did not restore the document")
    continue
  fi

  # 3. REDO RE-REACHES IT.
  after_command "$TMP" cli do redo >/dev/null 2>&1 || true
  snap "redone_$i" >/dev/null
  if ! cmp -s "$TMP/after_$i.json" "$TMP/redone_$i.json"; then
    FAILED=$((FAILED + 1))
    FAIL_LINES+=("$name: undo restored it but REDO did not re-reach the edit")
    continue
  fi

  PASS=$((PASS + 1))
  PASSING_NAMES="$PASSING_NAMES $name"
done

echo "  exercised ${#MUT_NAME[@]} command(s): $PASS fully undoable, $FAILED not"
for line in "${FAIL_LINES[@]:-}"; do [ -n "$line" ] && echo "      - $line"; done

# ---------------------------------------------------------------------------------------------
# THE RATCHET
# ---------------------------------------------------------------------------------------------
SORTED_NAMES="$(echo $PASSING_NAMES | tr ' ' '\n' | sort | tr '\n' ' ' | sed 's/^ *//;s/ *$//')"
if [ "$PASS" -eq "$EXPECTED_UNDOABLE" ] && [ "$SORTED_NAMES" != "$EXPECTED_NAMES" ]; then
  echo "  FAIL: the same NUMBER of commands is undoable, but not the same ONES."
  echo "        was: $EXPECTED_NAMES"
  echo "        now: $SORTED_NAMES"
  echo "        A count cannot see a swap. If this is a deliberate trade, update EXPECTED_NAMES"
  echo "        and say which command was given up and why."
  echo "undo_ratchet_check: FAIL"
  exit 1
fi
if [ "$PASS" -lt "$EXPECTED_UNDOABLE" ]; then
  echo "  FAIL: $PASS commands are undoable, down from $EXPECTED_UNDOABLE. Undo coverage REGRESSED."
  echo "        This is the one direction this check refuses. If a command was deliberately"
  echo "        reclassified, lower EXPECTED_UNDOABLE in this file and say why in the commit."
  echo "undo_ratchet_check: FAIL"
  exit 1
fi
if [ "$PASS" -gt "$EXPECTED_UNDOABLE" ]; then
  echo "  RATCHET: $PASS commands are undoable, up from $EXPECTED_UNDOABLE."
  echo "        Raise EXPECTED_UNDOABLE to $PASS in this file so the gain cannot be lost."
  echo "undo_ratchet_check: FAIL (ratchet needs raising — this is a good failure)"
  exit 1
fi
echo "undo_ratchet_check: PASS — $PASS undoable, ratchet holding at $EXPECTED_UNDOABLE"
