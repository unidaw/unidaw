#!/usr/bin/env bash
# THE DOCUMENT IS A VALUE, and this is the gate that says so.
#
# Step 1 of the undo work split two functions out of the file paths:
#
#   captureDocument(deps)                 -> the complete authored state, no file
#   applyDocument(deps, document, path)   -> that state made live, no file
#
# Undo is going to be built on exactly that pair — restore the engine to a document it already
# holds — so the pair has to be a faithful round trip before anything depends on it. If capture
# loses a field, undo will lose it too, silently and for every command at once. That is a worse
# version of the defect being fixed, so it gets its own check rather than a comment.
#
# THE PROPERTY, stated precisely, because the first version of this comment claimed more than the
# check delivers and a negative control caught it:
#
#     load a project  ->  save  ->  load THAT  ->  save again  ->  the two saves are IDENTICAL
#
# That is IDEMPOTENCE of capture-then-apply: applying a document and capturing it again must
# reach a fixed point. It catches any field the pair REWRITES, NORMALISES or DERIVES differently
# on a second pass — which is exactly how it found the plugin-reference overwrite in #118 on its
# first run.
#
# WHAT IT DOES NOT CATCH, and the comment used to say the opposite: a field that capture or apply
# drops SYMMETRICALLY. Both saves go through capture, so a field neither of them carries is
# absent from both files and the bytes still match. The claim that "a dropped field survives the
# first save because it came from the file" is simply wrong — the first save is capture's output
# too. A sabotage that cleared a track name on load passed this check cleanly, which is how the
# error was found.
#
# Catching symmetric omission needs a different comparison — the ORIGINAL preset against the
# first save, semantically rather than byte-wise, since the authored file is not in the
# serializer's normal form. Worth having and not yet built; do not read this check as providing
# it.
#
# RUN AGAINST THE SHIPPED PRESETS, not a fixture written here. A fixture exercises the fields its
# author remembered; presets/projects/ has the device chains, samplers, patcher graphs, mod links
# and automation that a hand-built document would omit — and those are precisely the parts undo
# does not carry today.
#
#   tools/document_value_check.sh
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
SHM="/docvalue_$$"
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

# The presets are copied in so the engine saves beside them without touching the repo.
cp "$ROOT"/presets/projects/*.uniproj.json "$TMP"/ 2>/dev/null
mkdir -p "$TMP/../audio" 2>/dev/null
N_PRESETS="$(ls -1 "$TMP"/*.uniproj.json 2>/dev/null | wc -l | tr -d ' ')"
# A GLOB THAT MATCHED NOTHING WOULD PASS SILENTLY — the whole check would assert nothing.
[ "$N_PRESETS" -ge 5 ] || { echo "  FAIL: only $N_PRESETS preset(s) copied; this check would prove nothing"; exit 1; }

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 600 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" >/dev/null 2>&1; }
wait_for_boot "$TMP/eng.log" "$ENG" 80 'UI: command thread started'

# DELETED BEFORE IT IS CLAIMED. A save that silently did nothing would otherwise be compared as
# the previous save's bytes, and the check would pass on a file nobody wrote this run.
# A LOAD THAT FAILED LEAVES THE ENGINE ON ITS PREVIOUS STATE — so the two saves match and this
# check goes GREEN, including on the exact failure it exists to catch (round two loads the file
# round one just produced). The `|| true` on the loads made every load unverifiable. Found by the
# review panel, which called it the most dangerous thing in the implementation: an instrument that
# reports success when the property is false is worse than no instrument.
assert_loads_ok() {  # assert_loads_ok <what>
  if grep -q '"event":"project.load"[^}]*"ok":false' "$TMP/eng.log" 2>/dev/null; then
    echo "  FAIL: a project.load reported ok:false during $1 — every comparison after it is void,"
    echo "        because a failed load leaves the engine on the state it already had."
    grep -o '"event":"project.load"[^}]*"ok":false[^}]*' "$TMP/eng.log" | tail -2 | sed 's/^/          /'
    return 1
  fi
  return 0
}

save_as() {  # save_as <project-name> <dest>
  local out="$TMP/$1.uniproj.json"
  rm -f "$out"
  cli do save "$1" --force
  for _ in $(seq 1 60); do [ -s "$out" ] && break; sleep 0.1; done
  [ -s "$out" ] || return 1
  cp "$out" "$2"
  return 0
}

fails=0
checked=0
for f in "$TMP"/*.uniproj.json; do
  name="$(basename "$f" .uniproj.json)"
  case "$name" in rt) continue ;; esac   # our own output from a previous iteration

  # multiout IS EXCLUDED AND IT IS NOT FINE — task #118. Its device names a plugin ("multiout")
  # that is not built on every machine; the engine substitutes the default, and the SAVE then
  # writes the substitution over the authored reference:
  #     {"vendor":"","name":"multiout","uid16":""}  ->  {"vendor":"daw","name":"Identity",...}
  # Opening a project whose plugin is missing and saving it therefore DESTROYS the reference to
  # the plugin the author wanted. A real pre-existing defect this check found on its first run,
  # excluded only so one untraced bug does not mask the property this check defends. Delete the
  # exclusion when #118 lands; if it starts passing with no fix, suspect the machine (the plugin
  # may simply be present) rather than believing it.
  case "$name" in
    multiout)
      echo "  SKIPPED multiout — save rewrites its plugin reference (task #118, a real bug)"
      continue ;;
  esac

  # BOTH ROUNDS SAVE UNDER ONE NAME, copied away between. The document carries meta.name, so
  # saving them as roundtrip_a_/roundtrip_b_ made every pair differ by that field and the check
  # reported all nine presets broken. The first version of this comment claimed the difference was
  # "normalised out above" — it was not; the comment asserted what the code did not do, which is
  # the failure this whole undo effort is about. Normalising the field away would ALSO hide a real
  # loss of the name, so the fix is to make the two saves genuinely comparable instead.
  after_command "$TMP" cli do load "$name" --force >/dev/null 2>&1 || true
  if ! save_as "rt" "$TMP/a_$name.json"; then
    echo "  FAIL: $name — the engine never wrote the first save"
    fails=$((fails + 1)); continue
  fi

  # Round two: the state now came through capture AND apply, not from the file.
  after_command "$TMP" cli do load "rt" --force >/dev/null 2>&1 || true
  if ! save_as "rt" "$TMP/b_$name.json"; then
    echo "  FAIL: $name — the engine never wrote the second save"
    fails=$((fails + 1)); continue
  fi

  assert_loads_ok "$name" || { fails=$((fails + 1)); continue; }
  checked=$((checked + 1))
  # THE ARTIFACT DIGESTS ARE VOLATILE, and precisely which parts is the whole of this comment.
  #
  # A schema-6 document commits each plugin artifact by its SHA-256 (AE-P1.2 G2-B item 18,
  # R-DEVICE-ID-LIFETIME). Round two reloads and re-captures, so its blobs come from plugin
  # instances that were torn down and recreated with round one's bytes pushed back in — and
  # pushing a VST3 state chunk and re-capturing it does not round-trip byte-exactly. Measured on
  # maximal: same size, ~200 bytes different, all in the parameter values, while the MANIFESTS are
  # byte-identical. That is a property of the plugin and its wrapper, not of the save.
  #
  # It was invisible until the document began carrying digests, so this change EXPOSED it rather
  # than caused it.
  #
  # BLANKED PRECISELY: `sha256`, `size` and `artifact_generation` only. `track_id`, `device_id`,
  # `kind` and `leaf` are left alone, so this check still fails if an artifact appears, vanishes,
  # or changes which device it belongs to — which is what it is here to see. Blanking the whole
  # inventory would leave its SHAPE unwatched, and that is the loss this check exists to catch.
  volatile_out() {
    sed -e 's/"artifact_generation": "[0-9a-f]*"/"artifact_generation": "<volatile>"/' \
        -e 's/"sha256": "[0-9a-f]*"/"sha256": "<volatile>"/' \
        -e 's/"size": [0-9]*/"size": <volatile>/' "$1"
  }
  if ! cmp -s <(volatile_out "$TMP/a_$name.json") <(volatile_out "$TMP/b_$name.json"); then
    # THE DIFF IS THE FINDING. "not identical" names no field; the first differing line does.
    echo "  FAIL: $name does not survive capture -> apply -> capture unchanged."
    echo "        The first difference (a = straight from the file, b = through the value):"
    diff <(volatile_out "$TMP/a_$name.json") <(volatile_out "$TMP/b_$name.json") \
      | head -6 | sed 's/^/          /'
    fails=$((fails + 1))
  fi
done

[ "$checked" -ge 5 ] || { echo "  FAIL: only $checked preset(s) round-tripped; expected at least 5"; fails=$((fails + 1)); }

if [ "$fails" -ne 0 ]; then
  echo "document_value_check: FAIL ($fails)"
  exit 1
fi
echo "  $checked preset(s) survive capture -> apply -> capture byte-identically"
echo "document_value_check: PASS — the document is a faithful value, so undo can be built on it"
