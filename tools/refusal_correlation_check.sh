#!/usr/bin/env bash
# A REFUSAL REACHES THE SENDER THAT CAUSED IT — END TO END, AGAINST A LIVE ENGINE.
#
# P2-CMD-00 mints a command id in the sender, carries it in `EventEntry::sampleTime`, and echoes it
# back in the refusal payload at offset 32. `tools/refusal_identity_check.sh` is the static half: it
# pins which payloads carry the field and refuses silent growth in that population. It cannot tell
# you the loop CLOSES. Reading the three files and seeing a mint, an echo and a match is not evidence
# that the value arriving is the value sent — that is the shape of claim this project has been wrong
# about most often.
#
# WHY THIS TEST CAN FAIL, which is the only interesting property of a test. `await_clip_outcome` has
# exactly two ways to recognise a refusal:
#
#   1. `command_id != 0 && refusal_id == command_id`  — the id path, what we are proving
#   2. `command_id == 0 && (track, commandType, sentBase) match` — the legacy triple, and it is
#      GUARDED on the caller having no id of its own
#
# Since `do note` now sends through `send_command_correlated`, `command_id` is never 0, so path 2 is
# unreachable from here. If the id does not survive the round trip, NEITHER path fires, the loop
# times out, and the outcome is `Unknown` — which the CLI deliberately treats as applied, because
# reporting a refusal it did not observe would be worse than silence.
#
# So a broken correlation does not look like an error. It looks like SUCCESS: exit 0 and
# `{"sent": "note"}` for an edit the engine threw away. That is exactly the failure this asserts
# against, and it is why the assertion is on the REFUSAL being reported rather than on any log line.
#
# THE REFUSAL IS FORCED, not waited for. `--base` presents a version the caller claims to have read
# earlier; pinning it also suppresses the automatic retry, because a caller who names a base is a
# concurrent author testing staleness and the refusal is the answer they asked for. One applied write
# moves the version, then a write against base 0 is stale by construction.
#
# BUT THE SETUP WRITE MUST BE PUBLISHED FIRST, and getting that wrong made this check fail in the
# shape of the very defect it detects. `await_clip_outcome` returns Applied as soon as the clip
# version MOVES, without asking which command moved it — so a setup publish still in flight is read
# by the stale write as its own success. It passed standalone and failed inside `ctest -j2`, where
# the engine publishes more slowly. A check whose false alarm is INDISTINGUISHABLE from its true
# finding is worse than no check, so the wait below observes the published version rather than
# sleeping.
#
# NEGATIVE CONTROL, run 2026-08-14 and reproducible in about four minutes:
#   cp apps/engine_ui_publish.cpp /tmp/pub.bak
#   # in emitClipReject, replace the two correlation writes with literal zeros:
#   #   payload.correlationLo = 0; payload.correlationHi = 0;
#   cmake --build build --target daw_engine -j8 && bash tools/refusal_correlation_check.sh
#   cp /tmp/pub.bak apps/engine_ui_publish.cpp   # cp, NOT `git checkout --`, which would revert to
#                                                # HEAD and delete any uncommitted work beside it
# RESULT, both directions observed rather than argued:
#   sabotaged  -> FAILED, and in the predicted SHAPE — not an error, but exit 0 with
#                 `{ "sent": "note", "base_version": 0 }` printed for an edit the engine refused.
#                 That is the whole reason this check asserts on the refusal being REPORTED.
#   restored   -> PASS, "the engine REFUSED this note — reason 1, presented base 0, engine holds 3"
#
# The restore used `cp` from a backup taken before the edit. `git checkout --` would have reverted to
# HEAD and destroyed the uncommitted work sitting beside it in the same tree.
#
#   tools/refusal_correlation_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="$ROOT/build/daw_engine"
CLI="$ROOT/ui/target/debug/daw-cli"
SHM="daw_corr_$$"

[ -x "$ENGINE" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
NOISE=""
cleanup() {
  if [ -n "$NOISE" ]; then
    kill "$NOISE" 2>/dev/null
    wait "$NOISE" 2>/dev/null
  fi
  # `wait` after the kill, so the shell reaps the child quietly. Without it the job-control notice
  # ("Terminated: 15") lands on the terminal AFTER the verdict line, which reads like a failure in
  # ctest output for a check that passed.
  if [ -n "$ENG" ]; then
    kill "$ENG" 2>/dev/null
    wait "$ENG" 2>/dev/null
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

echo "== booting engine (shm $SHM)"
( cd "$ROOT/build" && DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$ROOT/presets/projects" \
    ./daw_engine --run-seconds 60 >"$TMP/eng.log" 2>&1 ) &
ENG=$!

# Waits for the thread that dispatches commands, rather than sleeping a guessed interval.
for _ in $(seq 1 60); do
  grep -q "UI: command thread started" "$TMP/eng.log" 2>/dev/null && break
  kill -0 "$ENG" 2>/dev/null || { echo "engine died during boot"; tail -20 "$TMP/eng.log"; exit 1; }
  sleep 1
done
grep -q "UI: command thread started" "$TMP/eng.log" 2>/dev/null || {
  echo "engine never came up"; tail -20 "$TMP/eng.log"; exit 1; }

cli() { env DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }

cli do load maximal --force >/dev/null 2>&1 || { echo "  FAIL: load did not land"; exit 1; }

# ONE APPLIED WRITE FIRST. It moves the track's clip version, which is what makes base 0 stale. It
# also proves the send path works at all, so a later refusal cannot be confused with a dead engine.
if ! cli do note --track 0 --nanotick 0 --column 0 --pitch 60 >"$TMP/apply.log" 2>&1; then
  echo "  FAIL: the setup write was not applied — nothing here would be meaningful."
  cat "$TMP/apply.log"
  echo "refusal_correlation_check: FAILED"
  exit 1
fi
grep -q '"sent"' "$TMP/apply.log" || {
  echo "  FAIL: setup write reported no send:"; cat "$TMP/apply.log"
  echo "refusal_correlation_check: FAILED"; exit 1; }

# WAIT FOR THE SETUP WRITE TO BE PUBLISHED, and this wait is the whole reason this check is stable.
#
# `await_clip_outcome` returns Applied as soon as the track's clip version MOVES. It does not ask
# which command moved it. So if the setup write's publish is still in flight when the stale write
# starts waiting, the stale write sees the SETUP's version bump, concludes Applied, and the check
# fails — reporting exactly the message it prints when the id is genuinely lost.
#
# That is not hypothetical: this check passed standalone and failed inside a `ctest -j2` sweep, where
# the engine is slower to publish. The command thread finishing is not the consumer thread having
# published; the two are separated by a queue, and every timing-sensitive failure in this repository
# has lived in that gap. Waiting on the PUBLISHED version — the same value await_clip_outcome reads —
# closes it, and does so by observing the state rather than by sleeping long enough.
clip_version() {
  cli get tracks 2>/dev/null | grep '"track_id": 0,' \
    | grep -oE '"clip_version": [0-9]+' | grep -oE '[0-9]+$' | head -1
}
# QUIESCENCE, NOT "NON-ZERO". The first version of this waited for the published version to become
# greater than zero, which is a weaker condition than it looks: MEASURED, `load maximal` moves track
# 0's clip version 1 -> 2 over about half a second and then holds. Waiting for "> 0" therefore
# returned at 1, with the load's own second bump still in flight — and that bump landed during the
# stale write's wait, moved the version, and was read as the stale write's success.
#
# That is why waiting for the setup write to "be published" did not fix this: the setup write was
# never the thing still in flight. Six runs under concurrent load still failed three times, which is
# what sent me to measure the version instead of reasoning about it a second time.
#
# So the predicate is: the same value seen on several consecutive samples. That covers any producer
# of version bumps, including ones nobody has thought of, which a wait keyed to a specific command
# cannot.
wait_stable() {
  local prev="" same=0 v
  for _ in $(seq 1 120); do
    v="$(clip_version)"
    if [ -n "$v" ] && [ "$v" = "$prev" ]; then
      same=$((same + 1))
      [ "$same" -ge 5 ] && { echo "$v"; return 0; }
    else
      same=0
    fi
    prev="$v"
    sleep 0.1
  done
  echo "$prev"
  return 1
}

SETTLED="$(wait_stable)" || {
  echo "  FAIL: the published clip version never settled, so any write below would race whatever is"
  echo "        still moving it. Nothing can be concluded about correlation."
  echo "refusal_correlation_check: FAILED"; exit 1; }
if [ -z "$SETTLED" ] || [ "$SETTLED" -eq 0 ] 2>/dev/null; then
  echo "  FAIL: the setup write never became visible in the published clip version (read '$SETTLED')."
  echo "refusal_correlation_check: FAILED"
  exit 1
fi
echo "   setup write applied, published and settled (clip version $SETTLED)"

echo "== writing against a deliberately stale base"
cli do note --track 0 --nanotick 960000 --column 0 --pitch 62 --base 0 >"$TMP/stale.out" 2>"$TMP/stale.err"
RC=$?
cat "$TMP/stale.err"

# EXIT 3 IS "the engine REFUSED this note" — see the ClipOutcome::Refused arm in daw-cli. Exit 0
# means the CLI concluded Applied or Unknown, and Unknown is precisely the shape a broken
# correlation takes: no path matched, the wait timed out, and the tool reported success.
if [ "$RC" -eq 0 ]; then
  echo
  echo "  FAIL: the CLI reported SUCCESS for a write against a stale base."
  echo "        Both recognisers must have missed it. The legacy triple is guarded on the caller"
  echo "        having NO id, and this caller mints one, so the only path that can fire is"
  echo "        refusal_id == command_id. Reporting applied here means the id did not survive the"
  echo "        round trip: minted by the sender, carried in EventEntry::sampleTime, echoed by the"
  echo "        engine at payload offset 32, read back by await_clip_outcome."
  echo "        stdout was: $(cat "$TMP/stale.out")"
  echo "refusal_correlation_check: FAILED"
  exit 1
fi
if [ "$RC" -ne 3 ]; then
  echo "  FAIL: expected exit 3 (refusal reported), got $RC — the command failed for some other"
  echo "        reason and this check proved nothing about correlation."
  echo "refusal_correlation_check: FAILED"
  exit 1
fi
grep -q "REFUSED" "$TMP/stale.err" || {
  echo "  FAIL: exit 3 without the refusal message — the exit code and the report disagree."
  echo "refusal_correlation_check: FAILED"; exit 1; }


# THERE IS NO DETERMINISTIC GATE HERE FOR THE MASKING HALF, AND I TRIED.
#
# The other half of this ticket is that an unrelated version bump must not be mistaken for this
# command's success. I wrote a phase 2 for it: a second writer moving track 0's clip version
# continuously while the stale write waits. It passed WITH THE DEFECT DELIBERATELY RESTORED, so it
# was blind and it is gone rather than sitting here looking like coverage.
#
# Why it was blind: `await_clip_outcome` scans the diffs BEFORE checking the counter on every pass,
# so whenever the refusal arrives promptly the counter is never consulted and no amount of version
# churn can mask anything. The masking needs a DELAYED refusal, not a busy counter — which is why it
# first appeared under `ctest -j2` and not on an idle machine, and why a background writer cannot
# manufacture it.
#
# What the fix rests on instead, measured rather than asserted: the same stale write issued without
# waiting for the version to settle, six runs under concurrent ctest load.
#
#     counter deciding the outcome (before)   3 of 6 passed
#     command id deciding it (after)          6 of 6 passed
#
# That is real evidence and it is honest about being load-dependent. A probabilistic gate in ctest
# would fail on a fast machine and be silenced within a week.
echo
echo "refusal_correlation_check: PASS — the engine's refusal was matched to the id its sender minted"
echo "   (a broken echo would have surfaced as exit 0 and a reported success, not as an error)"
