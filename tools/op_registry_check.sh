#!/usr/bin/env bash
# Checks the OP REGISTRY (roadmap M2.21): every engine opcode has a CLI path, or is
# explicitly declared as having none.
#
# An op with no CLI path cannot be scripted, cannot be driven by an agent, and cannot be
# tested from a shell — so a feature reachable only by a keystroke is a feature that
# nothing in this repo can verify end to end. The failure is silent by construction: you
# add an opcode, wire the UI, and nothing anywhere says the other half is missing.
#
# The gap this found on its first run was 22 of 50 opcodes. Rather than pretend, the ones
# still missing are DECLARED below with the reason. The check fails when an opcode is
# neither reachable nor declared — so the gap can shrink, and cannot silently grow.
#
# Also asserts that every opcode has a NAME (uiCommandTypeName), because the history
# journal and every rejection diff identify ops by that name, and "op:unknown" in a
# journal is a record of something nobody can act on.
#
# Pure source analysis; no engine, no audio device.
#   tools/op_registry_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT" <<'PY'
import re, sys, pathlib

root = pathlib.Path(sys.argv[1])
payloads = (root / "apps/event_payloads.h").read_text()
cli = (root / "ui/daw-cli/src/main.rs").read_text()

m = re.search(r"enum class UiCommandType : uint16_t \{(.*?)\n\};", payloads, re.S)
if not m:
    print("  FAIL (setup): could not find the UiCommandType enum")
    raise SystemExit(1)
# EVERY enumerator, not only the ones with an explicit value. The old pattern required
# `= N`, so an opcode declared bare was invisible here: its missing CLI path and its
# missing name both went unreported by the check whose whole job is to notice them.
#
# A bare enumerator is now a FAILURE in its own right rather than something to tolerate and
# number implicitly. This enum is a WIRE CONTRACT shared with the frontend's UI on its own
# branch — an implicit value means inserting a line silently renumbers every opcode after
# it, on both sides, and the two only find out when a command starts doing something else.
enumerators = re.findall(r"^\s*([A-Za-z][A-Za-z0-9]*)\s*(=\s*(\d+))?\s*,",
                         m.group(1), re.M)
bare = [name for name, eq, _ in enumerators if not eq]
if bare:
    print("  FAIL: these opcodes are declared with no explicit value: %s"
          % ", ".join(bare))
    print("        The enum is a wire contract shared with the frontend. An implicit value")
    print("        means inserting a line renumbers every opcode after it on both sides,")
    print("        and nothing says so until a command starts doing something else.")
    raise SystemExit(1)
ops = [(name, value) for name, _, value in enumerators]
if len(ops) < 20:
    print("  FAIL (setup): parsed only %d opcodes; the enum shape must have changed"
          % len(ops))
    raise SystemExit(1)

# Opcodes with no CLI path, and WHY. Removing a line from here is how the gap shrinks;
# adding one is a deliberate act that shows up in review.
DECLARED_NO_CLI = {
    "None": "not an op — the zero value",
    "LoadPluginOnTrack": "addressed by scan index, which is not stable across machines; "
                         "`do add-device` with a vst_ref path is the durable equivalent",
    "SetAutomationTarget": "retargets an EXISTING automation clip to a different plugin "
                           "in the chain; `do automation --device D` sets that when the "
                           "clip is created, which covers the case that exists today",
    "SetDeviceEuclideanConfig": "the patcher node config surface as a whole is unbuilt "
                                "on the CLI side; see the patcher ops below",
    "BulkChunk": "a TRANSPORT, not a verb — it carries a fragment of some other command "
                 "and has no meaning on its own. EngineHandle::send_bulk chunks a payload "
                 "into these, and the commands that ride it (sampler-env-draw today) are "
                 "the things with CLI paths. A `do bulk-chunk` would only let a caller "
                 "hand-assemble a message the sender already builds correctly",
}

missing = []
undeclared = []
for name, value in ops:
    if f"UiCommandType::{name}" in cli:
        continue
    missing.append(name)
    if name not in DECLARED_NO_CLI:
        undeclared.append((int(value), name))

reachable = len(ops) - len(missing)
print("  %d of %d opcodes have a daw-cli path; %d declared as having none"
      % (reachable, len(ops), len(missing)))

ok = True
if undeclared:
    print("  FAIL: %d opcode(s) have no CLI path and are not declared:" % len(undeclared))
    for value, name in sorted(undeclared):
        print("        %3d %s" % (value, name))
    print("        Give it a `do` verb, or add it to DECLARED_NO_CLI with the reason.")
    ok = False

# A declaration for an op that HAS a path is stale, and a stale declaration is how a
# list like this stops describing reality.
stale = [n for n in DECLARED_NO_CLI if f"UiCommandType::{n}" in cli]
if stale:
    print("  FAIL: %d declaration(s) are stale — these DO have a CLI path now: %s"
          % (len(stale), ", ".join(sorted(stale))))
    print("        Remove them from DECLARED_NO_CLI.")
    ok = False
unknown = [n for n in DECLARED_NO_CLI if n not in {name for name, _ in ops}]
if unknown:
    print("  FAIL: %d declaration(s) name an opcode that no longer exists: %s"
          % (len(unknown), ", ".join(sorted(unknown))))
    ok = False

# Every opcode must have a readable name, or the history journal records "op:unknown"
# for something a person later has to act on.
namefn = re.search(r"inline const char\* uiCommandTypeName\(UiCommandType t\) \{(.*?)\n\}",
                   payloads, re.S)
if not namefn:
    print("  FAIL (setup): could not find uiCommandTypeName")
    ok = False
else:
    body = namefn.group(1)
    unnamed = [n for n, _ in ops if n != "None" and f"UiCommandType::{n}" not in body]
    if unnamed:
        print("  FAIL: %d opcode(s) have no name, so the history journal records them as"
              % len(unnamed))
        print("        \"op:unknown\": %s" % ", ".join(unnamed))
        ok = False
    else:
        print("  every opcode has a name for the history journal")

raise SystemExit(0 if ok else 1)
PY
rc=$?
[ "$rc" = "0" ] && echo "op_registry_check: PASS" \
                || { echo "op_registry_check: FAIL"; exit 1; }
