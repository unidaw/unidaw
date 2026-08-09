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
# registered. Registering them is a suite-policy call for the owner, and it is not free: three
# live-capture checks (sampler_default_sound, sound_addressed, realtime_pool) already ARE
# registered and red on every single run, and a permanently red suite is how a suite becomes
# furniture. The inconsistency between those three and these six is real and is flagged, not
# silently resolved in either direction.
#
# Note the reasons are all about the SUBJECT of the check, never about it being slow, awkward or
# occasionally flaky. A reason that could excuse anything excuses everything.
DECLARED_UNREGISTERED = {
    # EMPTY, AND THAT IS THE POINT OF THE LIST.
    #
    # It held six capture-dependent checks — level_match_bypass, master_fx, midi_per_bus, panic,
    # preview_note, sidechain — which assert on a LIVE capture and so need an output device that
    # runs its playback callback. This machine's did not for days: coreaudiod had been up 85 days
    # and the device opened, reported its rate, answered isPlaying() TRUE and never called back.
    #
    # coreaudiod was restarted on 2026-08-02, all six passed, and they are registered in ctest.
    # The list was built to SHRINK and not to grow silently, and this is it shrinking to nothing.
    #
    # Keep the mechanism. The next check that cannot run needs a reason here rather than a quiet
    # absence from CMakeLists, which is what forty-three checks had before this ratchet existed.
    #
    "undo_session_state_check.sh":
        "ITS ASSERTIONS ARE RIGHT AND ITS PROBE IS NOT — do not register until phase 1 is "
        "reliable. It guards a CONFIRMED and FIXED bug (undo used to reset the loop region; see "
        "task #123 item 5), and the fix is verified by hand in both directions plus a negative "
        "control. What is unreliable is the harness: phase 1 needs the undo to be VISIBLE before it "
        "reads the loop, and it anchors on a written note disappearing — but the note write is "
        "refused for a stale base (task #120) and `get notes --track 0` does not report the count "
        "this anchor assumes, so the phase reports 'the note never became visible (1 -> 1)'. "
        "IMPORTANT: an earlier version of this file PASSED WITH THE BUG PRESENT, because phase 1 "
        "expects the loop NOT to change and reading before the publish returns exactly that. Only "
        "the negative control exposed it. Registering it while the anchor is broken would put that "
        "vacuous pass back. Fix the anchor, re-run the control (restore the three loop stores into "
        "applyDocument and confirm phase 1 FAILS), then register.",
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
#
# AND A THIRD TIME, WHICH IS WHY THE RULE IS NOW A FUNCTION WITH A SELF-TEST BELOW. Both notes
# above describe a name being matched too LOOSELY, and both were fixed by tightening what counts.
# The rule that survived was `c in code` — a bare substring test — and it has the same disease in
# the one direction neither note considered: a check whose whole filename sits INSIDE a longer
# filename. `readback_check.sh` is a suffix of `note_readback_check.sh`, `track_readback_check.sh`,
# `automation_readback_check.sh` and `sampler_envelope_readback_check.sh`, all four registered.
#
# So tools/readback_check.sh — 182 lines, PASSING, covering the published readback of opcodes 10
# and 29 — was named in CMakeLists exactly ZERO times, ran in ctest never, and was reported by
# this ratchet as registered. It kept being maintained as though it ran: it was given the boot-wait
# treatment in 2ff0282, `exec` in 3b87fad and keep-evidence in 969750f. Three commits of care spent
# on a check that produced no result.
#
# That is this ratchet failing at precisely its own subject, so the fix is not only a better regex.
# The rule is a named function and it is exercised against synthetic inputs on EVERY run, including
# the case that broke it. A ratchet without a negative control is the thing it exists to prevent,
# and this file has now been that thing three times.
def strip_cmake_comments(text):
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))

def is_registered(name, code):
    # Anchored on the `tools/` path separator that every real registration carries
    # (`COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/<name>`), so a longer sibling filename
    # cannot contain it: "tools/note_readback_check.sh" does not contain "tools/readback_check.sh".
    # The trailing guard keeps an editor backup or a rename-in-progress — same filename with a
    # suffix glued on — from counting as a registration of the original.
    return re.search(r"tools/" + re.escape(name) + r"(?![\w.])", code) is not None

# THE NEGATIVE CONTROL, run every time rather than trusted once.
#
# EVERY NAME BELOW IS A CHECK THAT REALLY EXISTS, and that is a requirement rather than a
# convenience: doc_citation_check reads a bare `*_check.sh` anywhere in tools/ as a CITATION and
# fails when it names nothing, which is exactly what it should do. It caught the first version of
# this table, which used invented placeholder names — and then caught the comment that explained
# the fix, because that comment quoted the placeholders it was retiring. Real names cost nothing
# here: what is under test is the MATCHING RULE, not the names it is fed.
SELFTEST = [
    ("readback_check.sh", "COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/readback_check.sh", True,
     "a plain registration must count"),
    ("readback_check.sh", "COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/note_readback_check.sh",
     False, "a check whose name is a SUFFIX of a registered sibling must NOT count — the bug that "
            "hid tools/readback_check.sh"),
    ("readback_check.sh", "COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/readback_check.sh.bak",
     False, "a longer filename that merely starts with the name must NOT count"),
    ("lane_quantize_check.sh", "add_test(NAME lane_quantize COMMAND lane_quantize_tests)", False,
     "a unit binary sharing the stem must NOT count"),
]
selftest_failed = False
for name, code_frag, want, why in SELFTEST:
    if is_registered(name, code_frag) != want:
        print("  FAIL (self-test): is_registered(%r) returned %s, expected %s — %s"
              % (name, not want, want, why))
        selftest_failed = True
# The comment strip needs its own case for the same reason: it is a rule, so it can rot.
if strip_cmake_comments("# tools/ghost_check.sh\nadd_test(NAME x)").find("ghost_check") != -1:
    print("  FAIL (self-test): a check named only in a CMake COMMENT was not stripped")
    selftest_failed = True
if selftest_failed:
    print("        The rule this ratchet applies is broken, so its verdict on the real tree means")
    print("        nothing. That verdict is NOT reported below — a rule that fails its own cases")
    print("        passing the tree is the exact shape this check exists to catch.")
    raise SystemExit(1)

code = strip_cmake_comments(cmake)
registered = {c for c in checks if is_registered(c, code)}

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
