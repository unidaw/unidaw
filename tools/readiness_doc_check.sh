#!/usr/bin/env bash
# THE HEADER MAY NOT DESCRIBE A MODEL IT NO LONGER IMPLEMENTS.
#
# HOST-R1. apps/engine_readiness_level.h opened by describing a two-level model with a derived
# MirrorComplete, and the body below WITHDREW that level. A reader who stopped at the prologue learnt
# the model that had been removed — the same defect as a superseded rule stated two paragraphs above
# its replacement, which this project has now paid for in three separate documents.
#
# CHANGING A RULE MEANS CHANGING EVERY SENTENCE THAT STATES IT. So this asserts BOTH halves, because
# either alone is satisfiable by a file that says two things:
#
#   the SUPERSEDED wording is absent      — a "MirrorComplete" LEVEL, "MappedAndBypassed", and the
#                                           claim that hostReady means "mapped and bypassed"
#   the REPLACEMENT wording is present    — MappedAndDispatchable, the withdrawal reason, and the
#                                           three known limits the header must keep disclosing
#
# Checking only for the new wording is the mistake that let the stale prologue survive: the
# replacement WAS present, in the body, while the contradiction sat above it.
#
# Pure text analysis; no engine, no build.
#   tools/readiness_doc_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
H="$ROOT/apps/engine_readiness_level.h"
fail=0
note() { printf '  %s\n' "$*"; }

[ -f "$H" ] || { echo "readiness_doc_check: FAILED — $H does not exist"; exit 1; }

# ---- the superseded wording must be GONE ----------------------------------------------------
# Each pattern is a phrase that only makes sense under the withdrawn model. The withdrawal NOTE is
# allowed to name MirrorComplete — that is the point of a withdrawal — so the patterns below match
# the wording that ASSERTS it, not every mention of the word.
# A SUPERSEDED NAME MAY BE MENTIONED, BUT ONLY TO DISAVOW IT. My first version of this check
# forbade the WORD and immediately failed on the sentence
#
#     NOT "MappedAndBypassed", which is what the first version of this header called it.
#
# — a disavowal, which is the very thing a superseded-wording rule should want present. Forbidding
# the word rather than the ASSERTION is the same name-versus-property defect this whole file is
# about, committed inside the check written to catch it.
#
# So a line carrying a superseded name must also carry a disavowal token. That is still a proxy —
# nothing here parses English — and the honest limit is that a sentence could say NOT and then
# assert it anyway. It is bounded and checkable, which the bare word ban was not.
DISAVOWAL='NOT |not |previously|withdrawn|WITHDRAWN|first version|superseded|no longer'
while IFS='|' read -r pattern why; do
  [ -n "$pattern" ] || continue
  bare="$(grep -nE "$pattern" "$H" | grep -vE "$DISAVOWAL" || true)"
  if [ -n "$bare" ]; then
    fail=1
    note "FAIL  superseded wording ASSERTED (not disavowed): /$pattern/"
    note "      $why"
    printf '%s\n' "$bare" | head -3 | sed 's/^/        /'
  fi
done <<'EOF'
MappedAndBypassed|bypass is fire-and-forget (host_controller.cpp:595-601), so the engine never learns a plugin was bypassed; the level cannot assert it
MirrorComplete = [0-9]|the level was withdrawn, not renamed — a numbered enumerator reintroduces it
meant MAPPED-AND-BYPASSED|states that hostReady means bypassed, which the engine cannot observe
TWO-LEVEL HOST READINESS|the header models one readiness level plus a separate mirror question, not two levels of mirror state
EOF

# ---- the replacement wording must be PRESENT ------------------------------------------------
while IFS='|' read -r pattern why; do
  [ -n "$pattern" ] || continue
  if ! grep -qE "$pattern" "$H"; then
    fail=1
    note "FAIL  replacement wording missing: /$pattern/"
    note "      $why"
  fi
done <<'EOF'
MappedAndDispatchable|the level's current name, and the only claim about it the engine can observe
render_track.cpp:554|the re-arming site that makes a startup-sequence model wrong; without it the withdrawal has no reason
KNOWN LIMITS|the header must disclose what it does not model, or it reads as complete
HOST-R3|the non-atomic publication of generation/mapping/readiness must stay named
HOST-R4|the ABA limit of a 32-bit generation must stay named
EOF

if [ "$fail" -ne 0 ]; then
  echo "readiness_doc_check: FAILED"
  exit 1
fi
note "PASS  no superseded wording; all replacement wording and disclosed limits present"
echo "readiness_doc_check: PASS"
