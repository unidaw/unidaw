#!/usr/bin/env bash
# DOCS CITE SYMBOLS THAT EXIST, AND NEVER LINE NUMBERS.
#
# The docs used to cite `apps/daw_engine_main.cpp:8442`. Then that file lost 5,000 lines to a
# refactor and every one of the fifteen citations pointed somewhere else — :2172 landed on a
# logging line, :223 on a blank one, and several named a file the code had LEFT entirely. Nothing
# reported it, because a stale line number is still a valid-looking reference.
#
# So two rules, both checkable:
#
#   1. NO LINE-NUMBER CITATIONS. `file.cpp:1234` is a reference with a shelf life. A symbol name
#      survives every edit that does not delete the thing being cited — and if it IS deleted, that
#      is exactly when the doc should fail.
#   2. EVERY CITED SYMBOL EXISTS IN THE FILE IT IS CITED FROM. This is the half that catches code
#      MOVING rather than changing: after the command extraction, handleWriteAutomationPoint was
#      still real but no longer in daw_engine_main.cpp, and a citation naming both would now fail.
#
# Pure source analysis; no engine, no audio device.
#   tools/doc_citation_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT" <<'PY'
import os, re, sys, pathlib
root = pathlib.Path(sys.argv[1])
# ROOT DOCUMENTS ARE IN SCOPE TOO. This read `docs/*.md` plus README only, so ARCHITECTURE_
# REVIEW.md, AGENTS.md, SHM_LAYOUT.md and every other root-level document were invisible to
# it — and that is where the rot was found: the review document still described a header and two
# checks that v29 deleted, as present and covering things, while this check passed. (Their names
# are deliberately NOT repeated here: rule 3 below forbids a comment naming a path that does not
# exist, and it caught this very paragraph doing exactly that on the first run after the scope
# widened. The check working on its own author is the best evidence it works.)
# A citation checker that does not read the repository's largest design document is not checking
# citations, it is checking a directory.
docs = sorted(set(list((root / 'docs').rglob('*.md')) + list(root.glob('*.md'))))
ok = True

# ---- rule 1: the line-number citation count can SHRINK and cannot GROW.
#
# There are 42 of these already, in files this refactor never touched, and many are probably still
# accurate. Fixing all of them tonight is not honest scope; letting the number grow is not either.
# So this is the DECLARED_UNREGISTERED shape from check_registry_check.sh: a baseline that ratchets
# down. Rewrite one as a symbol citation and lower its number here; add one and this fails.
#
# The fifteen citations into daw_engine_main.cpp are already gone — that file lost a third of its
# lines tonight and every one of them had come to point somewhere else.
BASELINE = {
    # Measured with this rule's own regex each time the scope widened, never estimated.
    # docs/*.md and README, the original scope:
    'SAMPLER_DESIGN.md': 21, 'TRACKER_GAP_LIST.md': 20, 'per-note-ops.md': 1,
    # root documents, added 2026-08-14:
    'ARCHITECTURE_REVIEW.md': 19, 'MASTER_TRACK_DESIGN.md': 1,
    # docs/architecture/** task packets, added 2026-08-14. These are the documents designs are
    # READ FROM — the CMD00 design lives here — and nothing was checking their citations at all,
    # which is also why repository_integrity called them unclassified: a markdown file earns its
    # live provenance by being consumed by a registered check, and none consumed these.
    'P2-CMD-00-command-outcome.md': 4, 'P2-CMD-00-owner-decisions.md': 4,
    'P2-CMD-00-review.md': 3, 'P2-CMD-00-revised.md': 9,
    'P2-G4-01-inventory.md': 9, 'P2-HOST-R3-readiness-transaction.md': 13,
    'P2-HOST-R3b-decision.md': 10, 'P2-HOST-R3c-flapping-guard-race.md': 18,
    'P2-HOST-remediation.md': 5, 'P2-SHM-01-inventory.md': 10,
    'P2-WDOG-02-inventory.md': 12, 'P2-WDOG-03-ticket.md': 2,
    'P2-WDOG-04-watchdog-call-site.md': 16,
}
line_cite = re.compile(r'\b[\w/]+\.(?:cpp|h|rs|mjs):\d+')
for d in docs:
    if not d.exists():
        continue
    n = len(line_cite.findall(d.read_text(errors='ignore')))
    allowed = BASELINE.get(d.name, 0)
    if n > allowed:
        print("  FAIL: %s has %d line-number citation(s), baseline allows %d." % (d.name, n, allowed))
        print("        A line number is a reference with a shelf life: a refactor moves the code")
        print("        and the citation still looks valid. Cite the file and a symbol instead,")
        print("        then lower this doc's number in BASELINE.")
        ok = False
    elif n < allowed:
        print("  note: %s is down to %d line citations from %d — lower the baseline." % (d.name, n, allowed))

# ---- rule 2: a file/symbol citation must resolve, IN EITHER ORDER.
#
# BOTH ORDERINGS, because docs use both and checking one is checking most of them. row-ops.md
# wrote `(`renderTrack`, `apps/daw_engine_main.cpp`)` — symbol first — and that citation went
# stale the moment renderTrack moved to its own file, with this check passing the whole time. A
# rule that only recognises the spelling its author happened to use is not a rule about citations,
# it is a rule about that spelling. The same lesson as rule 3b below, which missed a bare filename
# for exactly the same reason and was caught by its own negative control.
pair = re.compile(r'`(apps/[\w/]+\.(?:cpp|h))`\s*\(`([A-Za-z_][\w:]*)')
pair_rev = re.compile(r'\(`([A-Za-z_][\w:]*)`,\s*`(apps/[\w/]+\.(?:cpp|h))`\)')
checked = 0
for d in docs:
    if not d.exists():
        continue
    text = d.read_text(errors='ignore')
    cites = [(m.group(1), m.group(2)) for m in pair.finditer(text)]
    cites += [(m.group(2), m.group(1)) for m in pair_rev.finditer(text)]
    for rel, sym in cites:
        f = root / rel
        if not f.exists():
            print("  FAIL: %s cites %s, which does not exist." % (d.name, rel))
            ok = False
            continue
        # the last component: TrackRuntime::patcherAudioChannels -> patcherAudioChannels
        needle = sym.split('::')[-1]
        # STRIP COMMENTS FIRST. Without this the check reads prose as code: a header whose
        # comment says "cutActiveNoteInColumn and cutAllActiveNotes were merged" satisfies a
        # citation of the symbol it says is GONE. That is the same defect that made a classifier
        # report a site as using `eventSample` because its comment read "offSample, NOT
        # eventSample" — comments are exactly where the warnings about the code live, so any tool
        # matching source as text will match the warning instead of the thing.
        body = re.sub(r'/\*.*?\*/', ' ', f.read_text(errors='ignore'), flags=re.S)
        body = re.sub(r'//[^\n]*', ' ', body)
        if needle not in body:
            print("  FAIL: %s cites `%s` in %s, and it is not there." % (d.name, sym, rel))
            print("        Either the symbol was renamed, or the CODE MOVED to another file —")
            print("        which is what a refactor does and what this check exists to notice.")
            ok = False
        checked += 1

# ---- rule 2b: a BARE path mention in a doc must exist, or be marked retired.
#
# Rule 2 matches only PAIRED citations — a path with a symbol beside it, in either order. A bare
# backticked path with no symbol beside it was matched by nothing, and that is precisely how three deleted
# artefacts sat in the review document being read as present: a header and two checks removed in
# v29, still named as covering things, with this check green.
#
# Measured before writing this: 152 backticked in-repo path mentions across the 21 scanned docs,
# of which 4 do not exist. The rule is affordable because the debt is small, and it was measured
# with this rule's own regex rather than a hand grep, which disagreed with it earlier today.
#
# THE RETIREMENT MARKER IS STRUCTURAL, NOT A WINDOW. A doc must be able to name a dead file on
# purpose — history is written about things that no longer exist — so a broken path is allowed if
# it is IMMEDIATELY followed by a parenthetical retirement note. "Immediately" means exactly that:
# closing backtick, optional spaces, then the note. It is deliberately not "somewhere in the next
# N characters", because a distance is the approximation this project keeps being bitten by; a
# nearby word would let an unrelated aside excuse a citation it never meant to.
#
# AND THE MARKER CANNOT BE A BLANKET EXEMPTION: marking a path that DOES exist as retired fails
# too. Otherwise the cheapest way to silence this rule is to declare everything dead, and the rule
# would decay into a formality nobody reads.
RETIRE = r'`\s*\((?:deleted|removed|retired)\b[^)]*\)'
bare_path = re.compile(r'`((?:apps|tools|ui|patcher_rust|platform_juce)/[\w/.\-]+\.(?:cpp|h|rs|sh|mjs|py))`')
bare_ok = 0
for d in docs:
    if not d.exists():
        continue
    text = d.read_text(errors='ignore')
    for m in bare_path.finditer(text):
        rel = m.group(1)
        marked = re.match(RETIRE, text[m.end() - 1:]) is not None
        exists = (root / rel).exists()
        if not exists and not marked:
            print("  FAIL: %s names %s, which does not exist and is not marked retired." % (d.name, rel))
            print("        If it is history, write it as `%s` (deleted in vNN) — naming a dead" % rel)
            print("        file on purpose is fine; naming one by accident reads as still present.")
            ok = False
        elif exists and marked:
            print("  FAIL: %s marks %s retired, and it exists." % (d.name, rel))
            print("        The marker is for files that are gone. Marking a live one turns this")
            print("        rule off for it, which is how a check becomes a formality.")
            ok = False
        else:
            bare_ok += 1

# ---- rule 3: a file path named in a COMMENT must exist, in tools/*.sh and apps/*.h too.
#
# Rules 1 and 2 only look at docs/. But the densest cross-references in this repo are not in the
# docs at all — they are in the banner comments on the ratchets and the headers, which routinely
# say "the assertions live in X" and "the rule underneath is Y". There are 353 such paths and
# nothing checked any of them.
#
# ADDED AT ZERO, on purpose. All 353 resolved the moment this was measured, so the baseline is
# 0 and there is no cleanup debt to carry — which is exactly why it is worth adding NOW. A check
# that has to start at "47 known-bad" gets read as noise; one that starts clean stays clean.
#
# The one that prompted it: a comment written minutes earlier cited a plausible-but-invented name
# for clip_anchor_meter_check.sh, and a reader would have concluded the check did not exist. The
# wrong name is deliberately NOT spelled out here, because rule 3b below would then flag this very
# comment for containing it — the same trap rule 2 strips comments to avoid.
#
# TWO SHAPES, because the first draft caught only one and its negative control said so. Ratchets
# are cited both WITH the tools/ prefix and by BARE filename, and a rule that demanded the prefix
# PASSED the sabotage that prompted it — the wrong name was bare. A check that cannot catch its
# own founding example is worth nothing, and only running the control could say so.
#
# Note that no example filename is written anywhere in this comment, real or invented. Rule 3b
# reads every comment line in this file too, so an illustrative placeholder is indistinguishable
# from a citation of something that does not exist. That is not a wart: a rule which must dodge
# its own text is telling you it cannot separate mention from use, and the honest response is to
# stop mentioning rather than to add an exemption for this file.
comment_line = re.compile(r'^\s*(?:#|//|\*)')
cite = re.compile(r'\b((?:apps|tools|ui|docs)/[\w./-]+\.(?:sh|cpp|h|md|mjs|rs))\b')
bare_check = re.compile(r'(?<![\w/])(\w+_check\.sh)\b')
bare_check_nosuffix = re.compile(r'(?<![\w/])(\w+_check)(?!\.sh)(?![\w.])')
paths = 0
# apps/*.cpp IS IN SCOPE TOO. It was not, and that is where a dead citation actually survived:
# main.cpp claimed "the property arrange_summary_check pins" long after that check was replaced.
# The rules below cost nothing extra on .cpp files — zero new failures when the glob was widened —
# and the one place a stale name had been sitting was in one.
for f in (sorted(root.glob('tools/*.sh')) + sorted(root.glob('apps/*.h'))
          + sorted(root.glob('apps/*.cpp'))):
    lines = f.read_text(errors='ignore').splitlines()
    for i, line in enumerate(lines, 1):
        if not comment_line.match(line):
            continue
        for rel in cite.findall(line):
            paths += 1
            if not (root / rel).exists():
                print("  FAIL: %s:%d names %s, which does not exist."
                      % (f.relative_to(root), i, rel))
                print("        A comment that points at a missing file reads as though the thing")
                print("        it describes was never built.")
                ok = False
        # ---- rule 3c: a ratchet named WITHOUT the .sh suffix must exist too.
        #
        # Ratchets are referred to both ways in this repo, and only one of the two was checked. The
        # suffixless form is the more common one in prose — "the property arrange_summary_check
        # pins" — and it is the form that survived after that check was deleted. A comment naming a
        # check nobody runs reads as coverage, which is worse than no comment.
        #
        # THE ESCAPE IS NARROW AND DELIBERATE: naming a dead check is legitimate when the text says
        # it is dead, which is how history gets recorded rather than quietly dropped. One of the
        # words below must appear within two lines of the mention.
        for name in bare_check_nosuffix.findall(line):
            if (root / 'tools' / (name + '.sh')).exists():
                paths += 1
                continue
            window = ' '.join(lines[max(0, i - 3):i + 2]).lower()
            if any(w in window for w in ('no longer', 'removed', 'replaced', 'deleted', 'gone')):
                continue
            print("  FAIL: %s:%d names %s, and tools/%s.sh does not exist."
                  % (f.relative_to(root), i, name, name))
            print("        A comment naming a check nobody runs reads as coverage. If the check is")
            print("        gone, say so on the same lines — 'no longer', 'removed', 'replaced',")
            print("        'deleted' or 'gone' within two lines makes it a record instead.")
            ok = False

        # ---- rule 3b: a BARE ratchet name resolves against tools/.
        for name in bare_check.findall(line):
            paths += 1
            if not (root / 'tools' / name).exists():
                print("  FAIL: %s:%d names %s, and there is no such check in tools/."
                      % (f.relative_to(root), i, name))
                print("        Ratchets are cited by bare name throughout this repo, so a wrong")
                print("        one reads as a check that exists and covers the case. It does not.")
                ok = False

if ok:
    print("  %d symbol citation(s) across %d docs all resolve; no line-number citations"
          % (checked, len([d for d in docs if d.exists()])))
    print("  %d file path(s) named in tool and header comments all exist" % paths)
raise SystemExit(0 if ok else 1)
PY
rc=$?
[ "$rc" = "0" ] && echo "doc_citation_check: PASS — every cited symbol exists where the doc says" \
                || { echo "doc_citation_check: FAIL"; exit 1; }
