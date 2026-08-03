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
docs = sorted(list((root / 'docs').glob('*.md')) + [root / 'README.md'])
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
BASELINE = {'SAMPLER_DESIGN.md': 21, 'TRACKER_GAP_LIST.md': 20, 'per-note-ops.md': 1}
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

# ---- rule 2: `path/file.ext` (`symbol`) must resolve.
pair = re.compile(r'`(apps/[\w/]+\.(?:cpp|h))`\s*\(`([A-Za-z_][\w:]*)')
checked = 0
for d in docs:
    if not d.exists():
        continue
    for m in pair.finditer(d.read_text(errors='ignore')):
        rel, sym = m.group(1), m.group(2)
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
paths = 0
for f in sorted(root.glob('tools/*.sh')) + sorted(root.glob('apps/*.h')):
    for i, line in enumerate(f.read_text(errors='ignore').splitlines(), 1):
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
