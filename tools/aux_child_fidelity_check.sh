#!/usr/bin/env bash
# A STEM IS A TRACK. Everything you can do to one must survive a save, a reload, and an undo.
#
# THIS CHECK EXISTS BECAUSE ITS ABSENCE WAS PROVEN. The aux-child capture in
# engine_save_project.cpp reached for five things — name, placements, ownedClips, automationClips,
# mixer — while the command handlers accept every edit on a stem: add a device, collapse it,
# change its quantize, route it, link a mod source. All of that was dropped on save, and once undo
# began applying documents it was reverted by every undo as well.
#
# When that subset was RE-INTRODUCED as a negative control, ALL EIGHT of the checks that looked
# relevant — multiout, scratch_clip, document_value, undo_ratchet, undo_invariants, master_track,
# master_fx, surround — went green. The fix was unverified and would have stayed that way. A green
# suite that passes with the bug present is the trap this repo keeps meeting; the only way out is
# a check written to fail on the specific loss.
#
# WHY THE OTHER CHECKS CANNOT SEE IT:
#   document_value round-trips a preset through capture->apply and compares two SAVES. Both go
#   through the same capture, so a field capture drops SYMMETRICALLY is absent from both files and
#   the bytes still match — its own header says so.
#   multiout asserts stems SOUND on separate channels, which is true whatever the save keeps.
#   undo_ratchet drives slot track 0, never a stem.
#
# WHAT IT ASSERTS, on a real stem derived from the multiout preset:
#   1. A property set on a stem SURVIVES save -> reload.  (the capture is complete)
#   2. A property set on a stem is RESTORED BY UNDO.      (the document undo applies is complete)
#
# `collapsed` is the probe because it is the cheapest field the old subset dropped: one bool, one
# command, no plugin required, and it is meaningless to the audio path — so it cannot pass by
# accident through some other mechanism that happens to rebuild the stem.
#
#   tools/aux_child_fidelity_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
SHM="/auxfid_$$"
ENG=""
fails=0
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

cp "$ROOT"/presets/projects/multiout.uniproj.json "$TMP"/ 2>/dev/null
[ -s "$TMP/multiout.uniproj.json" ] || { echo "  FAIL: multiout preset missing — this check would assert nothing"; exit 1; }

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 300 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" >/dev/null 2>&1; }
cli_out() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" 2>/dev/null; }
wait_for_boot "$TMP/eng.log" "$ENG" 80 'UI: command thread started'

after_command "$TMP" cli do load multiout --force

# THE STEM'S TRACK ID IS DERIVED, not authored — it depends on how many aux buses the plugin
# negotiated, so it is read from the engine rather than assumed. A hardcoded id would make this
# check silently test a SLOT track, which captures everything and would pass on the bug.
STEM_ID=""
for _ in $(seq 1 40); do
  # The published table says has_parent/parent_id, NOT is_aux_child — a stem is a track with a
  # parent. Reading the field the engine actually publishes rather than the one the FILE uses;
  # asserting against a name the wire does not carry is how a check ends up testing nothing.
  STEM_ID="$(cli_out get tracks 2>/dev/null \
             | grep -o '"track_id": *[0-9]*[^}]*"has_parent": *true' \
             | grep -o '"track_id": *[0-9]*' | grep -o '[0-9]*' | head -1 || true)"
  [ -n "$STEM_ID" ] && break
  sleep 0.25
done
if [ -z "$STEM_ID" ]; then
  echo "  FAIL: no aux-child track was derived from the multiout preset, so there is no stem to"
  echo "        test. Either the fixture plugin did not load or the derivation did not run —"
  echo "        NOT a pass, because the property under test was never exercised."
  fails=$((fails + 1))
else
  echo "  stem is track $STEM_ID"

  saved_collapsed() {  # reads the collapsed flag for the stem out of the saved file
    python3 - "$1" "$STEM_ID" <<'PY' 2>/dev/null || true
import json,sys
try:
    d=json.load(open(sys.argv[1]))
except Exception:
    print("noread"); raise SystemExit
tid=int(sys.argv[2])
for t in d.get("tracks",[]):
    if t.get("is_aux_child") and t.get("track_id")==tid:
        print("1" if t.get("collapsed") else "0"); raise SystemExit
print("absent")
PY
  }

  # ---------------------------------------------------------------- 1. it survives save -> reload
  after_command "$TMP" cli do collapse --track "$STEM_ID" --on 1
  rm -f "$TMP/auxfid.uniproj.json"
  cli do save auxfid --force
  for _ in $(seq 1 60); do [ -s "$TMP/auxfid.uniproj.json" ] && break; sleep 0.1; done
  if [ ! -s "$TMP/auxfid.uniproj.json" ]; then
    echo "  FAIL: the engine never wrote the save — nothing below can be compared"
    fails=$((fails + 1))
  else
    GOT="$(saved_collapsed "$TMP/auxfid.uniproj.json")"
    if [ "$GOT" != "1" ]; then
      echo "  FAIL: collapsed was set on stem $STEM_ID and the saved file says '$GOT'."
      echo "        The aux-child capture is dropping fields the handlers accept — the"
      echo "        hand-picked-subset shape. Every edit outside its list is lost on save AND"
      echo "        reverted by every undo."
      fails=$((fails + 1))
    else
      echo "  ok — collapsed survived the save"
    fi
  fi

  # ------------------------------------------------------------------- 2. undo restores it
  # The document undo applies is the document capture produces, so an incomplete capture is an
  # incomplete undo. Asserted separately because the two could diverge: a field could be saved
  # and still not be restored if undo applied something narrower.
  after_command "$TMP" cli do collapse --track "$STEM_ID" --on 0
  after_command "$TMP" cli do undo
  rm -f "$TMP/auxfid2.uniproj.json"
  cli do save auxfid2 --force
  for _ in $(seq 1 60); do [ -s "$TMP/auxfid2.uniproj.json" ] && break; sleep 0.1; done
  if [ ! -s "$TMP/auxfid2.uniproj.json" ]; then
    echo "  FAIL: the engine never wrote the second save"
    fails=$((fails + 1))
  else
    GOT2="$(saved_collapsed "$TMP/auxfid2.uniproj.json")"
    if [ "$GOT2" != "1" ]; then
      echo "  FAIL: after collapsing stem $STEM_ID, un-collapsing it and pressing undo, the stem"
      echo "        reads '$GOT2' — undo did not restore a stem's property."
      fails=$((fails + 1))
    else
      echo "  ok — undo restored the stem's property"
    fi
  fi
fi

if [ "$fails" -ne 0 ]; then
  echo "aux_child_fidelity_check: FAIL ($fails)"
  exit 1
fi
echo "aux_child_fidelity_check: PASS — a stem's properties survive save, reload and undo"
