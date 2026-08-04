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
import re, sys, pathlib, glob

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
    "RequestChainSnapshot": "DERIVED. It asks the engine to re-EMIT a chain that is "
                            "otherwise published on change, which a long-lived subscriber "
                            "needs after attaching to a running engine and a CLI never does: "
                            "`get chains` reads the published region directly, so there is no "
                            "edit it can have missed. A `do request-chain-snapshot` would ask "
                            "for a republish and then not be listening for it",
    "Quit": "IDENTITY. It means 'the last UI went away', which only the thing tracking UI "
            "connections can truthfully say — the sidecar sends it after a grace period so a "
            "page reload does not kill the session. daw-cli is not a UI, so from there the "
            "message would be a claim about session state it cannot observe, and the effect it "
            "wants (stop this engine) is what a signal already does",
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

# EVERY OPCODE THE ENGINE DISPATCHES BY PAYLOAD SIZE IS DECLARED AS CARRYING ITS OWN PAYLOAD.
#
# `uiCommandUsesGenericPayload()` decides whether the history journal may read a command's forty
# bytes as trackId / pitch / nanotick. For an opcode that packs a name, a paramId or a slot index
# into those same bytes, reading them that way records numbers that look like data and are not —
# which is precisely what that function's own comment warns about, immediately before claiming
# "The list below is now the full set; keep it that way when adding one."
#
# It was not. SIXTEEN opcodes were missing, including every sampler opcode, BulkChunk, SetRowOps,
# SetClipGrid and SetAudioClipField. The comment was true when written and false within a release,
# and nothing could tell. That is the argument for deriving the list instead of asserting its
# completeness in prose.
#
# DERIVED FROM THE ENGINE'S DISPATCH, which is what the comment says the list means: an opcode
# tested as `entry.size == sizeof(daw::Ui*Payload) && ... commandType == X` has its own payload by
# construction. Reading the header's struct defaults instead was tried first and is wrong — a
# payload shared by four opcodes (the marker commands) has no default naming any of them.
#
# ONE DIRECTION ONLY, and this is deliberate rather than laziness. Some opcodes carry their own
# payload and are dispatched on `payload.commandType` rather than on size — RequestWaveform,
# RequestClipWindow and SetDeviceParam — so they belong in the list and this derivation cannot see
# them. Failing on "listed but not derived" would demand their removal and make the journal worse.
# So the check can only ever demand ADDITIONS: the debt can shrink and cannot silently grow, the
# same rule as DECLARED_NO_CLI above.
# READ THE DISPATCHER WHEREVER IT LIVES. This used to name apps/daw_engine_main.cpp, and it went
# red the moment handleUiEntry moved to apps/engine_handle_ui_entry.cpp — not because anything was
# wrong, but because the derivation had lost sight of its subject. It said so rather than passing
# on an empty set, which is the only reason the move was noticed at all; a check keyed to a path
# is a check with an expiry date the refactor sets. Scanning the engine's sources means the next
# move costs nothing.
engine = "\n".join(p.read_text() for p in sorted(root.glob("apps/engine_*.cpp"))
                   ) + (root / "apps/daw_engine_main.cpp").read_text()
size_dispatched = set()
for m in re.finditer(r"entry\.size == sizeof\(daw::(Ui\w+)\)", engine):
    window = engine[m.end(): m.end() + 700]
    stop = window.find("{")
    size_dispatched.update(
        re.findall(r"daw::UiCommandType::(\w+)", window[:stop] if stop > 0 else window))

genericfn = re.search(
    r"inline bool uiCommandUsesGenericPayload\(UiCommandType t\) \{(.*?)\n\}", payloads, re.S)
if not genericfn:
    print("  FAIL (setup): could not find uiCommandUsesGenericPayload")
    ok = False
elif not size_dispatched:
    # A derivation that finds nothing would pass silently for ever, which is the failure this
    # whole file exists to prevent.
    print("  FAIL (setup): no size-dispatched opcodes found in the engine — the pattern this")
    print("        derives from has changed, so this assertion is no longer measuring anything")
    ok = False
else:
    gbody = genericfn.group(1)
    gcut = gbody.find("return false;")
    declared_own = set(re.findall(r"case UiCommandType::(\w+):", gbody[:gcut] if gcut >= 0 else gbody))
    order = {name: int(value) for name, value in ops}
    missing_own = sorted(size_dispatched - declared_own, key=lambda n: order.get(n, 0))
    if missing_own:
        print("  FAIL: %d opcode(s) are dispatched by their own payload struct and are NOT listed"
              % len(missing_own))
        print("        in uiCommandUsesGenericPayload, so the history journal reads their bytes")
        print("        as trackId/pitch/nanotick — numbers that look like data and are not:")
        for n in missing_own:
            print("          %-4s %s" % (order.get(n, "?"), n))
        print("        Add each to that switch.")
        ok = False
    else:
        print("  every size-dispatched opcode is declared as carrying its own payload (%d)"
              % len(size_dispatched))

# ---------------------------------------------------------------- HANDLER REACH.
#
# An opcode can have a value, a name and a CLI verb and still do NOTHING, because none of those
# three is the code that acts on it. Dispatch is 34 size-gated blocks that return, then one long
# else-if chain, spread across engine_handle_ui_entry.cpp and the 16 command modules. Until this
# commit that chain had NO TERMINAL ELSE: an opcode with no arm fell off the bottom and returned
# normally, and the history journal still recorded it as accepted — because the journal is written
# from the fact that a command ARRIVED, not from anything dispatch did with it. Write path green
# end to end, state never moved.
#
# The terminal else now makes that loud AT RUNTIME (DAW_EVENT "ui.op_unhandled"), which needs the
# op to actually be sent to be seen. This is the same property asserted STATICALLY, so an opcode
# added to the enum with no dispatch code at all fails at source-analysis time, before anyone has
# to think to exercise it.
#
# NAMED IN A DISPATCH SOURCE is deliberately weaker than "correctly handled" — proving the latter
# from source is what the end-to-end checks are for. This catches the case that actually happens:
# the enum entry lands, the handler is forgotten, and nothing says so.
NO_HANDLER_NEEDED = {
    "None": "not an op — the zero value the enum starts from",
}
dispatch_sources = [p for p in sorted(glob.glob(str(root / "apps/*.cpp")) +
                                      glob.glob(str(root / "apps/*.h")))
                    if "event_payloads.h" not in p and "tests_main" not in p]
dispatch_text = "\n".join(open(p).read() for p in dispatch_sources)
if len(dispatch_sources) < 40:
    print("  FAIL (setup): found only %d dispatch source(s); the parse has stopped seeing the"
          % len(dispatch_sources))
    print("        code it searches, which reads identically to every opcode being handled")
    ok = False
else:
    unhandled = [n for n, _ in ops
                 if n not in NO_HANDLER_NEEDED
                 and not re.search(r"UiCommandType::%s\b" % re.escape(n), dispatch_text)]
    if unhandled:
        print("  FAIL: %d opcode(s) are named nowhere in any dispatch source:" % len(unhandled))
        for n in unhandled:
            print("          %-4s %s" % (dict(ops).get(n, "?"), n))
        print("        An opcode with no dispatch arm is ACCEPTED and JOURNALLED and then does")
        print("        nothing, silently and forever. Add a handler, or declare it in")
        print("        NO_HANDLER_NEEDED with the reason it needs none.")
        ok = False
    else:
        print("  every opcode is named in a dispatch source (%d checked, %d declared as needing"
              % (len(ops) - len(NO_HANDLER_NEEDED), len(NO_HANDLER_NEEDED)))
        print("  no handler)")

raise SystemExit(0 if ok else 1)
PY
rc=$?
[ "$rc" = "0" ] && echo "op_registry_check: PASS" \
                || { echo "op_registry_check: FAIL"; exit 1; }
