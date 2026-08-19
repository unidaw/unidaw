#!/usr/bin/env python3
"""Every host-slot index must come from host_slot_rule.h, not from a counter in a device loop.

AE-P1.2 G2-B step 4, P-EXECUTION-AUTHORITY-CONSUMERS.

WHY THIS EXISTS RATHER THAN A ROUND OF CONVERSIONS. "Which plugin does host slot N mean" was
implemented independently in THIRTEEN places, and they had drifted into at least four live defects:
a bypass addressed to the wrong plugin, plugin state saved from the wrong slot, meters attributed to
the wrong device, and a chain snapshot requesting another device's bus layout. Converting thirteen
sites by hand leaves the fourteenth to be written next month. The ledger already records this exact
shape once, under "one rule, seven sites" -- and its lesson was to guard the SHAPE when a rule keeps
being re-implemented, which nothing did, which is why it happened again at thirteen.

WHAT IT LOOKS FOR, AND WHAT THAT IS NOT. It matches a variable named like a host index being
incremented. That is a NAME-AND-INCREMENT HEURISTIC, not a structural detector, and it therefore
both over- and under-matches:

  - It OVER-matches: a plain `for (size_t hostIndex = 0; ...; ++hostIndex)` over a vector is not a
    counter in a device loop. The first run hit exactly that in engine_load_project.cpp and I nearly
    tightened the pattern to silence it -- and the site was REAL. The vector it indexes is built by
    `vstIds`, a kind-only walk, so `savedIds[hostIndex]` maps VST-kind position onto a host slot and
    restores plugin state to the wrong plugin whenever anything ahead does not resolve. A hit
    matched for the wrong reason still has to be READ before it is dismissed.
  - It UNDER-matches: an ordering established by push_back with no counter at all -- `vstIds` itself
    -- is invisible to it. Forty-seven raw `kind == Vst*` tests exist across eighteen files, and most
    are legitimate filters rather than ordinal walks, so widening the pattern to all of them would
    fail for reasons unrelated to the defect.

SO THIS IS A RATCHET, NOT A CENSUS, and saying which it is matters: it can tell you that no NEW
hand-rolled host-index walk appeared, and it cannot tell you that none exists. The OWED list below is
the measured population as of 2026-08-19, not the output of this predicate.

CONVERTED SITES ARE NOT LISTED HERE. There is no allowlist of "these are fine": a site either uses
the rule or it appears below as owed. An allowlist is how a temporary exemption becomes permanent,
and this file would then be measuring its own list instead of the tree.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")

# A counter whose NAME says it addresses a host, incremented in a loop. Both spellings, because
# `x++` and `++x` are the same construct and a check that sees only one is a check that teaches
# people which one to write.
COUNTER = re.compile(r"\b(host(?:Index|Idx|Slot)\w*)\s*\+\+|\+\+\s*\b(host(?:Index|Idx|Slot)\w*)\b")

# The owed population, measured on 2026-08-19. Each entry is a site that still derives a host index
# by hand. This list may only SHRINK: the check fails if a file not named here grows such a walk,
# and fails if a file named here no longer has one (so a fixed site cannot stay on the list and
# quietly license a new walk in the same file).
OWED = {
    "engine_rt_helpers.cpp": "resolvability without the Direct branch -- modulation link targets.",
    "engine_device_commands.cpp": "resolvability without the Direct branch -- open editor, set param.",
    "engine_request_commands.cpp": "resolvability without the Direct branch -- params read-back.",
    "engine_produce_block.cpp": "resolvability without the Direct branch -- host segment start/length "
                                "on the RT path.",
    "engine_track_setup.cpp": "walks the chain to seed host state at setup.",
    "engine_load_project.cpp": "`vstIds` builds an ORDERED list of VST-kind device ids with no "
                              "resolvability test, and the loop below indexes it as a host slot -- "
                              "so restored plugin state lands on the wrong plugin whenever anything "
                              "ahead of it does not resolve. Found because this check matched its "
                              "for-loop induction variable, which is the wrong reason for a real "
                              "site; see the heuristic note above.",
}


def reads_the_authority(text: str, match) -> bool:
    """Is this host-index name ITERATING the recorded mapping rather than deriving one?

    `runtime.hostSlotDevices` is the mapping the host was actually built with, recorded by
    rebuildHostForChain. A loop over it is the CURE this check exists to push people towards, and it
    naturally spells its induction variable `hostIndex` because that is exactly what the index is.
    Flagging that would make the check punish its own remedy, and the only way to satisfy it would be
    to rename the variable — silencing by spelling, which teaches nothing and hides the next real one.

    So the distinction is READING the authority versus REBUILDING it, and it is drawn from the
    statement the match sits in rather than from the file it sits in. This is deliberately NOT an
    allowlist of converted files: a file that reads the mapping in one place and hand-rolls a walk in
    another is still flagged for the second.
    """
    line_start = text.rfind("\n", 0, match.start()) + 1
    line_end = text.find("\n", match.end())
    if line_end == -1:
        line_end = len(text)
    return "hostSlotDevices" in text[line_start:line_end]


def main() -> int:
    found = {}
    for name in sorted(os.listdir(APPS)):
        if not name.endswith((".cpp", ".h")) or name.endswith("_tests_main.cpp"):
            continue
        text = open(os.path.join(APPS, name), encoding="utf-8").read()
        hits = [m for m in COUNTER.finditer(text) if not reads_the_authority(text, m)]
        if hits:
            found[name] = len(hits)

    new = sorted(set(found) - set(OWED))
    fixed = sorted(set(OWED) - set(found))

    if new:
        print("FAIL: a new hand-rolled host-index walk appeared:")
        for name in new:
            print(f"        apps/{name}")
        print("      Ask host_slot_rule.h instead -- hostIndexOf for one device,")
        print("      assignHostSlotOccupancy to walk them all. A counter in a device loop is the")
        print("      construct that produced four live defects; it is not a style preference.")
        return 1

    if fixed:
        print("FAIL: these are recorded as owed but no longer have a hand-rolled walk:")
        for name in fixed:
            print(f"        apps/{name}")
        print("      Remove them from OWED. A stale entry licenses a NEW walk in the same file,")
        print("      because this check would then see it and say nothing.")
        return 1

    total = sum(found.values())
    print(f"host_index_walk_check: PASS -- {len(found)} file(s) still derive a host index by hand "
          f"({total} counter site(s)), all recorded as owed")
    for name in sorted(found):
        print(f"    apps/{name}: {OWED[name]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
