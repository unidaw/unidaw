#!/usr/bin/env bash
# IS WHAT YOU ARE TESTING BUILT FROM WHAT YOU ARE READING?
#
# C++ and Rust are separate builds over a shared contract. Change a payload, rebuild one side,
# and the other keeps its old idea of the struct — and a stale side HAS NO SYMPTOM OF ITS OWN.
# It does not crash and it does not warn: it simply does not know about the field, so the feature
# looks dead and every test around it passes. The failure is too TOTAL to read as a version skew,
# which is exactly why it gets misdiagnosed as the feature being broken.
#
# It cost me twice in one night — a command arriving at an engine that had never heard of the
# opcode, and a read-back field that was always zero — and cost the web-UI agent three times.
# Their idea, and the good part of it is the last paragraph below.
#
# COMPARED AGAINST THE CONTRACT FILES SPECIFICALLY, not against "anything in the repo". A guard
# that fires whenever any source file is newer than a binary fires constantly, and a guard that
# fires constantly is one nobody reads. These files are the ones where a mismatch is silent:
# everything else announces itself as a compile error or a behaviour change you can see.
#
# EACH BINARY IS COMPARED AGAINST THE SOURCES IT IS ACTUALLY BUILT FROM, which is not what the
# first version did and the difference matters. It compared every binary against the newest of
# ALL the contract files, C++ and Rust together — so a C++-only edit marked daw-cli stale, and
# `cargo build` cannot clear that: cargo does not relink a crate whose Rust sources have not
# changed, so the binary's mtime never advances and the check demands something impossible. A
# check that cannot be made green is one you learn to ignore, which is the same failure as one
# that fires constantly.
#
# The cross-language case still lands, because a real contract change touches BOTH sides: the C++
# header and its Rust mirror move together, and then whichever binary you forgot to rebuild is
# older than its own source. What is gone is only the false half.
#
# WHAT THIS DOES NOT CHECK: that the two mirrors AGREE. layout.rs asserts its struct sizes against
# hardcoded numbers, so it catches a Rust-side slip but only if someone hand-updated the number to
# match the C++ — nothing compares the two languages' actual layouts. That is a real gap and a
# different check.
#
# FOUND ON ITS FIRST RUN: juce_host_process 244 minutes stale, because event_payloads.h had been
# using EventId without including it — fine in daw_engine, which happened to include event_id.h
# first, and a hard compile error in the host. The host had been failing to build for four hours
# and nothing said so, because only daw_engine was ever being rebuilt.
#
#   tools/contract_freshness_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Each group is "sources -> binaries built from them". Semicolon-separated, space between fields.
# NOT named GROUPS: that is a bash builtin array holding the current user's unix group ids, and
# assigning to it is silently ignored. The first run of this loop iterated over the group id 20.
CONTRACT_GROUPS=(
  # The C++ contract: the published regions and kShmVersion, every command payload and opcode,
  # and the per-track engine<->host protocol.
  "apps/shared_memory.h;apps/event_payloads.h;apps/ipc_protocol.h::build/daw_engine;build/juce_host_process::cmake --build build -j8"
  # The Rust mirror of all of it.
  "ui/daw-bridge/src/layout.rs;ui/daw-bridge/src/control.rs::ui/target/debug/daw-cli::cargo build --manifest-path ui/Cargo.toml -p daw-cli"
)

mtime() { stat -f %m "$1" 2>/dev/null || stat -c %Y "$1" 2>/dev/null; }

STALE=0
for g in "${CONTRACT_GROUPS[@]}"; do
  srcs="${g%%::*}"; rest="${g#*::}"
  bins="${rest%%::*}"; cmd="${rest#*::}"

  NEWEST=0; NEWEST_FILE=""
  IFS=';' read -ra SRC <<<"$srcs"
  for f in "${SRC[@]}"; do
    [ -f "$ROOT/$f" ] || continue
    t="$(mtime "$ROOT/$f")"
    if [ -n "${t:-}" ] && [ "$t" -gt "$NEWEST" ]; then NEWEST="$t"; NEWEST_FILE="$f"; fi
  done
  [ "$NEWEST" -gt 0 ] || { echo "  no contract files found for '$srcs' — check the paths"; exit 2; }

  IFS=';' read -ra BIN <<<"$bins"
  for b in "${BIN[@]}"; do
    if [ ! -x "$ROOT/$b" ]; then
      # Missing is not stale. A check that needs it will say so in its own words, and some of
      # these are optional depending on what you are working on.
      echo "  $b: not built (skipped)"
      continue
    fi
    t="$(mtime "$ROOT/$b")"
    if [ "${t:-0}" -lt "$NEWEST" ]; then
      echo "  $b: STALE by $(( (NEWEST - t) / 60 )) minute(s) against $NEWEST_FILE"
      echo "      $cmd"
      STALE=$((STALE + 1))
    else
      echo "  $b: current"
    fi
  done
done

if [ "$STALE" -gt 0 ]; then
  echo
  echo "  FAIL: $STALE binary/binaries are older than the contract they are built from."
  echo
  echo "        A stale side of the contract has NO symptom of its own — it does not know about"
  echo "        the field, so the feature looks dead and every test around it passes. Every"
  echo "        failure downstream of here would be a lie about a different subsystem."
  exit 1
fi

echo "contract_freshness_check: PASS — every binary is built from its current contract"
