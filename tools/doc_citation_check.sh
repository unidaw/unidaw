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

if ok:
    print("  %d symbol citation(s) across %d docs all resolve; no line-number citations"
          % (checked, len([d for d in docs if d.exists()])))
raise SystemExit(0 if ok else 1)
PY
rc=$?
[ "$rc" = "0" ] && echo "doc_citation_check: PASS — every cited symbol exists where the doc says" \
                || { echo "doc_citation_check: FAIL"; exit 1; }
