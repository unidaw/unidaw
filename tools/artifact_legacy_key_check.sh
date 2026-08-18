#!/usr/bin/env bash
# A LEGACY MANIFEST THAT NAMES A DIFFERENT DEVICE MUST FAIL THE LOAD, NOT BE RESTAMPED.
#
# AE-P1.2 G2-B item 18, `artifact_presence_matrix.present_file_rules`, quoted whole because the
# parenthetical is the half that matters here:
#
#   "an existing non-regular or unreadable path, an empty blob, malformed manifest JSON, or
#    manifest whose embedded track/device differs from the expected source key (LegacyArtifactKey
#    for schema 1-5, indexed global key for schema 6) fails load before document or
#    ExecutionSnapshot publication."
#
# WHY THIS CHECK EXISTS. `legacy_import` also says the importer must "rewrite manifest embedded ids
# in memory", and an implementation that does the rewrite WITHOUT the comparison satisfies that
# sentence while destroying this one: a `t0_d1.params.json` whose body says track 3 device 9 —
# copied in from another project, or left behind by a rename — is silently restamped to (0,1),
# retained, and republished with a matching digest. After that every load verifies it as correct.
# The mismatch is not merely undetected, it is ERASED, and the only moment it can ever be seen is
# the one load where the file still disagrees with its own name.
#
# A review found exactly that defect in the first implementation. This is the control that would
# have caught it.
#
# THE SHAPE IS: load a legacy project (PASS), corrupt only the embedded pair (must FAIL), restore
# it (must PASS again). The restore is not decoration — without it a check that refuses everything
# looks identical to a check that refuses the right thing.
#
#   tools/artifact_legacy_key_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
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
ENG=""
fail() { echo "  FAIL: $*"; exit 1; }

# A SHIPPED LEGACY PROJECT, chosen because it is real rather than authored for this check: `demo`
# is schema 4 and carries one hosted device with both sides on disk under the flat names.
SRC="$ROOT/presets/projects/demo.uniproj.json"
SRCSTATE="$ROOT/presets/projects/demo.uniproj.state"
[ -f "$SRC" ] || { echo "  SKIP: presets/projects/demo.uniproj.json is not present"; exit 0; }
[ -d "$SRCSTATE" ] || { echo "  SKIP: demo has no .state directory"; exit 0; }
MANIFEST_SRC="$(ls "$SRCSTATE"/*.params.json 2>/dev/null | head -1)"
[ -n "$MANIFEST_SRC" ] || { echo "  SKIP: demo ships no parameter manifest"; exit 0; }

cp "$SRC" "$TMP/legacykey.uniproj.json"
cp -R "$SRCSTATE" "$TMP/legacykey.uniproj.state"
MANIFEST="$TMP/legacykey.uniproj.state/$(basename "$MANIFEST_SRC")"
cp "$MANIFEST" "$TMP/manifest.good"

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="/alk_$$" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 90 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 80 "UI: command thread started" || fail "the engine never booted"

# `project.load` carries ok=true/false, so the ANSWER is read off the engine's own event rather
# than off the CLI's exit code, which reports whether the command was delivered.
# `grep -c` PRINTS ITS ANSWER AND EXITS NON-ZERO WHEN THAT ANSWER IS ZERO. So `|| echo 0` appends
# a SECOND line to a count that already said 0, and every later `[ "$n" -gt "$m" ]` dies with
# "integer expression expected" — which this check did, on its first run, ninety-nine times before
# reporting that the engine had never loaded anything. It had; the waiter was broken, not the load.
# `|| true` swallows the status without adding output.
load_count() { grep -c '"event":"project.load"' "$TMP/eng.log" 2>/dev/null || true; }

load_says() {
  local want="$1" label="$2"
  local before
  before="$(load_count)"
  DAW_UI_SHM_NAME="/alk_$$" "$CLI" do load legacykey --force >/dev/null 2>&1 || true
  local waited=0
  while [ "$waited" -lt 150 ]; do
    local now
    now="$(load_count)"
    [ "$now" -gt "$before" ] && break
    sleep 0.2
    waited=$((waited + 1))
  done
  [ "$waited" -lt 150 ] || fail "$label: the engine never reported a project.load at all"
  local got
  got="$(grep '"event":"project.load"' "$TMP/eng.log" | tail -1)"
  case "$got" in
    *'"ok":'"$want"*) return 0 ;;
    *) fail "$label: expected ok=$want, engine said: $got" ;;
  esac
}

load_says true "a legacy project with an honest manifest"
echo "  the untouched legacy project loads"

# CORRUPT ONLY THE EMBEDDED PAIR. The filename is left alone on purpose: this is precisely the
# case where the file's NAME and its CONTENTS disagree, which is the only thing being tested.
python3 - "$MANIFEST" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s2 = re.sub(r'"track": \d+', '"track": 3', s, count=1)
s2 = re.sub(r'"device": \d+', '"device": 9', s2, count=1)
if s2 == s:
    sys.exit("the manifest did not contain an embedded pair to corrupt")
open(p, "w").write(s2)
PY
[ $? -eq 0 ] || fail "could not corrupt the manifest"
cmp -s "$MANIFEST" "$TMP/manifest.good" && fail "the corruption changed nothing — the control is blind"

load_says false "a legacy manifest whose embedded pair does not match its filename"
grep -q 'names track 3 device 9' "$TMP/eng.log" || \
  fail "the load failed, but not for the mismatch — the message must name what disagreed"
echo "  a manifest naming track 3 device 9 under a different filename is REFUSED"

# THE POSITIVE CONTROL. Without it, an engine that refused every load would pass everything above.
cp "$TMP/manifest.good" "$MANIFEST"
load_says true "the restored manifest"
echo "  and the restored manifest loads again"

echo "artifact_legacy_key_check: PASS — the embedded pair is compared before it is rewritten"
exit 0
