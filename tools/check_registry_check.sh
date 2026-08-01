#!/usr/bin/env bash
# EVERY CHECK EITHER RUNS, OR IS DECLARED AS NOT RUNNING AND WHY.
#
# `tools/` held 101 *_check.sh and CMakeLists registered 58 of them. The other 43 ran only when
# somebody remembered them by name. Thirty-seven of those PASSED — they were not excluded for a
# reason, they were never wired up — so they were working checks that could have rotted at any
# time with nothing reporting the absence of a result.
#
# The web-UI agent found 27 of their own suites unrun on the same day, and three of them passing
# on capture files 39 hours old. Same defect, both sides of the repo, found the same way: by
# asking what is NOT being run rather than by reading what is.
#
# So this is the DECLARED_NO_CLI shape from op_registry_check, one level up: a check is either
# registered in ctest, or listed below with the reason it cannot be. The list can SHRINK and
# cannot silently GROW — adding a check now forces the decision at the moment it is added.
#
# Pure source analysis; no engine, no audio device.
#   tools/check_registry_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT" <<'PY'
import os, re, sys, pathlib

root = pathlib.Path(sys.argv[1])
cmake = (root / "CMakeLists.txt").read_text()

# ------------------------------------------------------------------ the declared exclusions.
#
# TWO THINGS ARE TRUE OF ALL SIX, and the first version of this file recorded only the second,
# which flattened a considered engineering boundary into "the machine is broken":
#
#   1. They are on REAL HARDWARE BY DESIGN. tools/offline_render_check.sh keeps the ledger of
#      what renders and what does not, and every one of these appears there with its own reason —
#      quoted below rather than paraphrased, because a summary of a reason is how a reason rots.
#      Seven other checks WERE moved to offline render and are registered and green; these six
#      were considered at the same time and deliberately kept.
#   2. They therefore cannot answer on THIS machine, whose output device never runs its callback
#      (`afplay -v 0` fails with AudioQueueStart -66681, coreaudiod up 85+ days).
#
# So this list does NOT shrink when audio is fixed here — that only makes them RUNNABLE, not
# registered. Registering them is a suite-policy call for Jaakko, and it is not free: three
# live-capture checks (sampler_default_sound, sound_addressed, realtime_pool) already ARE
# registered and red on every single run, and a permanently red suite is how a suite becomes
# furniture. The inconsistency between those three and these six is real and is flagged, not
# silently resolved in either direction.
#
# Note the reasons are all about the SUBJECT of the check, never about it being slow, awkward or
# occasionally flaky. A reason that could excuse anything excuses everything.
DECLARED_UNREGISTERED = {
    "level_match_bypass_check.sh": "toggles bypass MID-RUN, which a batch render cannot time",
    "master_fx_check.sh":          "its subject is the one-block-latency host on the master sum",
    "midi_per_bus_check.sh":       "types a note onto a DERIVED child at runtime; moving it into "
                                   "the fixture would route it through aux-child persistence "
                                   "instead of the runtime edit path it exists to test",
    "panic_check.sh":              "interactive: sends a command MID-PLAYBACK",
    "preview_note_check.sh":       "interactive: sends a command MID-PLAYBACK",
    "sidechain_check.sh":          "cross-track pull under a real clock",
}

checks = sorted(p.name for p in (root / "tools").glob("*_check.sh"))
if not checks:
    print("  FAIL (setup): no *_check.sh found in tools/ — this assertion is measuring nothing")
    raise SystemExit(1)

# REGISTERED means CMakeLists names the SCRIPT, not that some test happens to share its stem.
# Matching on the stem would count `lane_quantize` (a unit binary) as registering
# lane_quantize_check.sh, which is exactly the confusion that let 43 scripts look wired up.
#
# And it must be named in CMAKE CODE, not in a CMAKE COMMENT. The first version of this file
# matched the whole text and passed on ITSELF, because the header comment I added above the 37
# new entries says "DECLARED in tools/check_registry_check.sh". A ratchet whose own registration
# is satisfied by prose about it is measuring nothing: comments are exactly where stale claims
# accumulate, which is the reason this ratchet exists.
code = "\n".join(l for l in cmake.splitlines() if not l.lstrip().startswith("#"))
registered = {c for c in checks if c in code}

ok = True
unregistered = [c for c in checks if c not in registered]
undeclared = [c for c in unregistered if c not in DECLARED_UNREGISTERED]
if undeclared:
    print("  FAIL: %d check(s) are not registered in ctest and not declared here:"
          % len(undeclared))
    for c in undeclared:
        print("          %s" % c)
    print("        A check nobody runs is a check that rots, and it rots silently because")
    print("        nothing reports the absence of a result. Register it in CMakeLists, or add")
    print("        it to DECLARED_UNREGISTERED with the reason it cannot run.")
    ok = False

# A declaration for a check that IS registered, or that no longer exists, is the stale half — the
# same failure the persisted-field ratchet guards against, where a table looks complete while
# covering something that is not there.
stale = [c for c in DECLARED_UNREGISTERED if c in registered]
if stale:
    print("  FAIL: %d declaration(s) are stale — these ARE registered now: %s"
          % (len(stale), ", ".join(sorted(stale))))
    print("        Remove them from DECLARED_UNREGISTERED so the list keeps meaning something.")
    ok = False

gone = [c for c in DECLARED_UNREGISTERED if c not in checks]
if gone:
    print("  FAIL: %d declaration(s) name a check that no longer exists: %s"
          % (len(gone), ", ".join(sorted(gone))))
    ok = False

if ok:
    print("  %d checks: %d registered in ctest, %d declared as unable to run"
          % (len(checks), len(registered), len(DECLARED_UNREGISTERED)))
    for c, why in sorted(DECLARED_UNREGISTERED.items()):
        print("      not run: %-30s %s" % (c, why))

raise SystemExit(0 if ok else 1)
PY
rc=$?
[ "$rc" = "0" ] && echo "check_registry_check: PASS — every check either runs or says why not" \
                || { echo "check_registry_check: FAIL"; exit 1; }
