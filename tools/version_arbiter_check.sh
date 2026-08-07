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
# nothing passes them all. These numbers are the counts when the check was written; the point is
# not the exact value but that finding FEWER means the extraction broke, not that the code got
# safer. Renaming an arbiter is exactly how that would happen.
if [ "$n_clip" -lt 7 ] || [ "$n_harm" -lt 2 ]; then
  echo "FAIL: found $n_clip clip-arbitrated and $n_harm harmony-arbitrated commands (expected >=7 and >=2)."
  note "The extraction found less than it did when written, so every check below would pass"
  note "on an empty set. An arbiter was probably renamed — fix the greps, do not lower these."
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

if [ "$fail" -ne 0 ]; then
  echo "FAILED"
  exit 1
fi
echo "ALL PASS"
