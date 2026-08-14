#!/usr/bin/env bash
#
# THE SIDECAR KEEPS A SECOND COPY OF THE ENGINE'S SCOPE RULE. This checks the two AGREE ON
# ANSWERS — which is not the same as agreeing on contents, and the difference is the whole point.
#
# Three counters arbitrate edits in this project:
#
#   harmonyTimeline.harmonyVersion        WriteHarmony, DeleteHarmony
#   the GLOBAL clipVersion                commands listed in uiCommandIsGlobalScope
#   the per-track trackClipVersion        everything else that names a track
#
# The engine decides with `uiCommandIsGlobalScope` (apps/event_payloads.h). The sidecar decides
# with `is_harmony_scope` + `is_global_scope` (ui/daw-sidecar/src/main.rs) so `resolve_base` can
# stamp a base for a client that did not supply one. Two hand-written copies of one rule, in two
# languages — the shape that has cost this project more than any other.
#
# ── WHY THIS DOES NOT JUST DIFF THE TWO LISTS ────────────────────────────────────────────────
#
# They differ LEGITIMATELY and always will. The C++ list names 22 commands; only NINE are ever
# arbitrated at all, and for the other 13 the classification is dead code that no counter is ever
# chosen by. A list-equality check would demand the sidecar mirror 13 entries that decide nothing,
# and would go red on every unrelated addition to the C++ switch — a ratchet nobody can keep, which
# is a ratchet that gets deleted.
#
# So this derives the ARBITRATED SET from the engine's own call sites and checks only those. That
# is the set where a disagreement changes an answer. For each one, the sidecar must route it to the
# same counter the engine will compare it against:
#
#   harmony-arbitrated   -> must be in is_harmony_scope
#   clip-arbitrated, global-scope in C++  -> must be in is_global_scope
#   clip-arbitrated, track-scoped in C++  -> must be in NEITHER
#
# ── WHAT IT WOULD HAVE CAUGHT ───────────────────────────────────────────────────────────────
#
# `resolve_base` routed WriteHarmony to a CLIP counter while the engine compared it against the
# HARMONY counter. Every key change written without an explicit base was refused unless the two
# happened to be equal. It survived because both sides looked right in isolation: the C++ listed
# WriteHarmony as global-scope, the Rust comment said harmony "has its own counter entirely and
# never reaches this" — true of the arbiter it described, false of the function relying on it.
#
# BOTH SIDES ARE PARSED FROM SOURCE. Neither is a list maintained here, because a list kept in the
# checker is a THIRD copy and would rot the same way the first two did.

set -uo pipefail
cd "$(dirname "$0")/.."

CPP_HDR=apps/event_payloads.h
RUST=ui/daw-sidecar/src/main.rs
fail=0
note() { printf '  %s\n' "$*"; }

# ── The arbitrated set, taken from the engine's call sites ──────────────────────────────────
# Definitions and test binaries excluded: the definition site names no command, and a test may
# arbitrate a command the product never does.
collect() {
  local fn="$1" skip="$2" f
  for f in apps/*.cpp; do
    case "$f" in *tests_main.cpp|$skip) continue;; esac
    grep -A4 "$fn(" "$f" 2>/dev/null | grep -oE 'UiCommandType::[A-Za-z]+'
  done | sed 's/UiCommandType:://' | sort -u
}

CLIP_ARB=$(collect requireMatchingClipVersion '*clip_edit.cpp')
HARM_ARB=$(collect requireMatchingHarmonyVersion '*harmony_timeline.cpp')

n_clip=$(printf '%s\n' "$CLIP_ARB" | grep -c '[A-Za-z]')
n_harm=$(printf '%s\n' "$HARM_ARB" | grep -c '[A-Za-z]')

# BLINDNESS FLOOR. Every assertion below is "for each arbitrated command", so a grep that matches
# nothing passes them all. Finding FEWER usually means the extraction broke rather than that the
# code got safer, and renaming an arbiter is exactly how that would happen.
#
# LOWERED 7 -> 5 ON 2026-08-14, AND THE REASON MATTERS BECAUSE THE INSTRUCTION BELOW SAYS NOT TO.
# The floor said "do not lower these", so it was only movable by establishing that the population
# genuinely shrank rather than that the grep went blind. It did, and here is the evidence:
#
#   * the set at this check's own introduction (7a9d3f6a) was 7; at HEAD it is 5;
#   * the two missing members are Undo and Redo;
#   * they were removed by 6d1a20b9, "undo Step 2c: the switchover — 4 of 15 undoable becomes
#     15 of 15", which deleted both `requireMatchingClipVersion(payload.baseVersion, ...Undo/Redo)`
#     calls when undo became a whole-document operation;
#   * a structural extraction that reads each call's ARGUMENTS, rather than this proximity window,
#     agrees at 5 — so the window is not what shrank the count.
#
# AND THE REMOVAL WAS RIGHT IN KIND. AE-P1.2's ruling R10 reached the same conclusion from the other
# direction: undo replaces the whole document through applyDocument, so a PER-TRACK clip version is
# the wrong instrument for it — the thing being replaced is not a track. Arbitrating it against one
# would refuse edits that are fine and permit the ones that are not.
#
# THE RESIDUAL HAZARD IS REAL AND IS NOT THIS CHECK'S. An edit made between a user seeing the screen
# and pressing undo is still silently reverted, and covering that needs a DOCUMENT-level version,
# which does not exist. R10 says so explicitly and it is tracked there, not here.
#
# This check was red from 6d1a20b9 until today. It stayed invisible because it sat behind the `-E`
# exclusion filters used for the broad sweeps in this programme — a red check hidden by the tooling
# meant to find red checks.
if [ "$n_clip" -lt 5 ] || [ "$n_harm" -lt 2 ]; then
  echo "FAIL: found $n_clip clip-arbitrated and $n_harm harmony-arbitrated commands (expected >=5 and >=2)."
  note "The extraction found less than it did when written, so every check below would pass"
  note "on an empty set. An arbiter was probably renamed — fix the greps, do not lower these"
  note "without establishing, as the comment above does, that the population itself shrank."
  exit 1
fi

# ── The C++ global-scope list ────────────────────────────────────────────────────────────────
CPP_GLOBAL=$(awk '/inline bool uiCommandIsGlobalScope/,/^}/' "$CPP_HDR" \
             | grep -oE 'UiCommandType::[A-Za-z]+' | sed 's/UiCommandType:://' | sort -u)

# ── The sidecar's two lists ──────────────────────────────────────────────────────────────────
rust_list() {
  awk "/^fn $1\(/,/^}/" "$RUST" | grep -oE 'UiCommandType::[A-Za-z]+' | sed 's/UiCommandType:://' | sort -u
}
RS_HARM=$(rust_list is_harmony_scope)
RS_GLOBAL=$(rust_list is_global_scope)

has() { printf '%s\n' "$2" | grep -qx "$1"; }

echo "version arbiter agreement"
echo "  engine arbitrates $n_clip commands on a clip counter, $n_harm on the harmony counter"

# ── 1. Harmony-arbitrated commands must be routed to the harmony counter ────────────────────
for c in $HARM_ARB; do
  if has "$c" "$RS_HARM"; then
    note "PASS  $c -> harmony counter on both sides"
  else
    fail=1
    note "FAIL  $c is arbitrated against harmonyVersion by the engine, but the sidecar's"
    note "      is_harmony_scope does not list it — resolve_base will hand it a CLIP version,"
    note "      which matches only by coincidence and is refused in silence otherwise."
  fi
done

# ── 2. Clip-arbitrated commands must match the engine's own global/track split ───────────────
for c in $CLIP_ARB; do
  if has "$c" "$RS_HARM"; then
    fail=1
    note "FAIL  $c is arbitrated against a CLIP counter, but the sidecar routes it to harmony."
    continue
  fi
  if has "$c" "$CPP_GLOBAL"; then
    if has "$c" "$RS_GLOBAL"; then
      note "PASS  $c -> global clip counter on both sides"
    else
      fail=1
      note "FAIL  $c is global-scope in uiCommandIsGlobalScope, so the engine compares it against"
      note "      the GLOBAL clipVersion — but the sidecar omits it from is_global_scope and will"
      note "      quote a per-track version. The two diverge on the first edit."
    fi
  else
    if has "$c" "$RS_GLOBAL"; then
      fail=1
      note "FAIL  $c is track-scoped in the engine, but the sidecar lists it as global-scope and"
      note "      will quote the global counter against a per-track comparison."
    else
      note "PASS  $c -> per-track counter on both sides"
    fi
  fi
done

# ── 3. A NEW SEND PATH IS THE WAY THIS GETS MISSED ──────────────────────────────────────────
#
# The classification above was RIGHT for SetClipText and the rename was still refused, because the
# cliptext path builds its own `UiClipTextHeader` and ships it with `send_bulk` — it never reached
# `resolve_base` at all. Getting the rule right does not help a caller that does not consult it.
#
# There is no clean syntactic test for "consults resolve_base": the payload built in
# `build_command` is resolved by its CALLERS, several frames away, so proximity gives false
# positives and a whole-file grep gives false negatives. So this does the honest thing instead —
# it pins the number of send paths. All 10 below were read by hand on 2026-08-07:
#
#   3251 send_harmony_and_await   resolved (harmony counter)
#   4192 Stop, last client gone   not arbitrated
#   4205 Quit, last client gone   not arbitrated
#   4948 LoadProject              not arbitrated
#   4996 chord, batch             resolved per track
#   5002 build_command, batch     resolved
#   5179 SamplerSetSlotName       not arbitrated, carries no base
#   5256 cliptext                 resolved  <- the one that was missed
#   5606 chord, single            resolved
#   5624 build_command, single    resolved
#
# THE RECEIVER IS NOT ALWAYS CALLED `handle`. This first counted `handle.send_*`, reported 8, and
# its own negative control — a send appended through a binding named `h` — did not trip it. The two
# real sites at 4192/4205 use `h`, so they had never been in the audited set at all while the list
# above claimed they were. A guard keyed to an incidental variable name is blind in precisely the
# way the thing it guards was. Both turned out benign (Stop and Quit are not arbitrated), which is
# luck, not vindication.
#
# If this count changes, ONE of two things is true and both need a person: a send path was added
# (audit whether its command is arbitrated, and route it through `resolve_base` if so), or one was
# removed (drop it from the list above). Adjusting the number without doing that is how the next
# one gets missed.
SEND_SITES=$(grep -cE '\.send_command\(|\.send_bulk\(|\.send_chord_command\(' "$RUST")
if [ "$SEND_SITES" -ne 10 ]; then
  fail=1
  note "FAIL  the sidecar has $SEND_SITES engine-send paths; 10 were audited when this was written."
  note "      A send path that builds its own payload can bypass resolve_base entirely — that is"
  note "      exactly how SetClipText stayed broken while its classification was correct."
  note "      Audit the new one, then update the count and the list in this file."
else
  note "PASS  10 engine-send paths, the number audited"
fi

if [ "$fail" -ne 0 ]; then
  echo "FAILED"
  exit 1
fi
echo "ALL PASS"
