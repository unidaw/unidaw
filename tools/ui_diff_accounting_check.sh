#!/usr/bin/env bash
# EVERY WRITE TO THE UI-OUT RING GOES THROUGH sendUiDiff, so every DROP is counted.
#
# A diff that does not fit the ring is discarded, not blocked — the writer is on the command thread
# and must never wait on a UI that is not draining. That makes a delivered diff and a discarded one
# identical from the caller's side, and the only evidence is uiDiffSent/uiDiffDropped and a
# rate-limited log line. sendUiDiff is what maintains them.
#
# SIX EMITTERS USED TO CALL daw::ringWrite DIRECTLY AND THROW THE RESULT AWAY. A refusal that did
# not fit vanished twice over: the UI never learned its command was rejected, and nothing counted
# the loss. They produced the same bytes, so nothing anywhere could tell the difference — the shape
# this repo keeps paying for is exactly this, a second copy of a rule that agrees on the bytes and
# differs in the behaviour.
#
# THIS IS A RULE, NOT A COMMENT, because the next person adding an emitter will copy the emitter
# next to it. Whichever one they copy, this check makes the wrong one fail.
#
# The ringStd writes are deliberately NOT covered: that is a per-track ring with its own semantics
# and its own consumer, and the diff counters do not describe it.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/apps/engine_ui_publish.cpp"
ok=1

if [ ! -f "$SRC" ]; then
  echo "ui_diff_accounting_check: FAIL — $SRC is missing. If the module was renamed, this check"
  echo "        must be repointed; a check that cannot find its subject is not a passing check."
  exit 1
fi

# ---- The rule: no statement in this file may call ringWrite on the UI-out ring. sendUiDiff itself
# lives in the header, which is why looking only at the .cpp is the right scope.
# ANY mention, not only a bare statement. The first version of this rule looked for a line STARTING
# with daw::ringWrite, and emitClipReject wrote `if (daw::ringWrite(ringUiOut, entry)) {` — a
# hand-written copy of sendUiDiff's whole body, counters and drop log included, which the check
# walked straight past. A rule that matches a shape rather than the thing it forbids is the same
# defect as the code it is guarding.
BAD="$(grep -n 'daw::ringWrite(ringUiOut' "$SRC" || true)"
if [ -n "$BAD" ]; then
  echo "ui_diff_accounting_check: FAIL — $(printf '%s\n' "$BAD" | wc -l | tr -d ' ') write(s) to the"
  echo "        UI-out ring bypass sendUiDiff, so their drops are neither counted nor logged:"
  printf '%s\n' "$BAD" | sed 's/^/          apps\/engine_ui_publish.cpp:/'
  echo "        Use sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload) — it builds the"
  echo "        same entry makeUiDiffEntry did, and maintains uiDiffSent/uiDiffDropped."
  ok=0
fi

# ---- BLINDNESS GUARD. If nothing in this file publishes to the UI-out ring any more, the rule
# above passes on an empty set — which looks exactly like a file that obeys it. Say so instead.
SENDS="$(grep -c 'sendUiDiff(deps, ringUiOut' "$SRC" || true)"
if [ "${SENDS:-0}" -lt 4 ]; then
  echo "ui_diff_accounting_check: FAIL — only ${SENDS:-0} call(s) to sendUiDiff(deps, ringUiOut)"
  echo "        found, and there should be several. Either the emitters moved out of this file or"
  echo "        the pattern changed; either way this check is guarding nothing and must be"
  echo "        repointed rather than left green."
  ok=0
fi

if [ "$ok" = "1" ]; then
  echo "ui_diff_accounting_check: PASS — $SENDS UI-out publish(es), all through sendUiDiff"
  exit 0
fi
exit 1
