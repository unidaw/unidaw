#!/usr/bin/env bash
# WHICH LAMBDA IS ACTUALLY CHEAP TO LIFT OUT OF main()? — an instrument, not a gate.
#
# Four functions have now been moved out of main() into their own translation units, and the
# ordering I used for the first three was LINE COUNT. That was wrong, and the fourth proved it:
# handleAssembledBulk was 446 lines with ELEVEN captures, while loadProjectFromPath was twice the
# size with FIVE TIMES the entanglement. The cheap one sat behind two expensive ones for no reason
# other than that it looked smaller on a list sorted by the wrong column.
#
# What a verbatim extraction actually costs is the CAPTURE SET: every `[&]` capture becomes a
# member of a Deps struct, an argument at the call site, and a re-binding line in the new function.
# Lines move for free — they are copied unchanged. So the number to sort by is captures, and the
# ratio worth looking at is captures per hundred lines: how much interface you buy per unit of
# main() removed.
#
# HOW IT MEASURES: empties one lambda's capture list to `[]` and compiles. Every name the body uses
# is then an error — "cannot be implicitly captured" — and clang lists them all. It works on a COPY;
# apps/daw_engine_main.cpp is never written to, so an interrupted run cannot leave the tree edited.
#
# -ferror-limit=0 IS NOT OPTIONAL. clang stops at 20 errors by default, and the first "complete"
# capture list I took this way was really 23 items long. A truncated list looks exactly like a short
# one.
#
# AND IT UNDERCOUNTS, ALWAYS, IN ONE SPECIFIC WAY. Constants are exempt from capture, so a
# `constexpr` or const-initialised local that the body uses is NOT reported — it surfaces only when
# the extracted translation unit fails to compile. That has now happened twice (kPlacementUnchanged
# and patternTicks). Read every number here as a lower bound.
#
# NOT REGISTERED IN ctest. It is a planning tool: it runs a full compile per candidate, so a sweep
# costs minutes, and its output is an ordering rather than a pass/fail.
#
#   tools/extraction_cost.sh [min-lines]        default 100
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
MIN="${1:-100}"
SRC="$ROOT/apps/daw_engine_main.cpp"

[ -d "$BUILD" ] || { echo "configure the build first"; exit 2; }

# The compile command comes from the build system rather than being written out here, so it cannot
# drift away from how the file is really compiled. compile_commands.json is the reliable source:
# `make -n` prints nothing once the object is up to date, and `make -W` does not propagate through
# CMake's generated sub-makefiles, so both report "no compiler" for a perfectly healthy tree.
CC="$(python3 -c "
import json, sys
try:
    db = json.load(open('$BUILD/compile_commands.json'))
except OSError:
    sys.exit(0)
for e in db:
    if e['file'].endswith('apps/daw_engine_main.cpp'):
        print(e['command']); break
")"
if [ -z "$CC" ]; then
  echo "  no compile command for daw_engine_main.cpp in $BUILD/compile_commands.json."
  echo "  Re-configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON. Hard-coding the include paths here"
  echo "  instead would be exactly the kind of second copy that goes stale silently."
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
COPY="$TMP/probe.cpp"

# ---- find the named lambdas inside main() and their brace spans
python3 - "$SRC" "$MIN" > "$TMP/cands.txt" <<'PY'
import re, sys
src = open(sys.argv[1]).read().split('\n')
mn = int(sys.argv[2])
mi = next(i for i, l in enumerate(src) if re.match(r'^int main\(', l))
out = []
i = mi
while i < len(src):
    m = re.match(r'^\s*(?:const\s+)?auto\s+(\w+)\s*=\s*(?:\[|$)', src[i])
    if m and ('[&]' in src[i] or (i + 1 < len(src) and '[&]' in src[i + 1])):
        d, st, j = 0, None, i
        while j < len(src) and j < i + 4000:
            d += src[j].count('{') - src[j].count('}')
            if st is None and '{' in src[j]:
                st = j
            if st is not None and d == 0 and j > st:
                if j - i + 1 >= mn:
                    out.append((j - i + 1, m.group(1), i + 1))
                break
            j += 1
    i += 1
for n, name, ln in sorted(out, reverse=True):
    print('%d %s %d' % (n, name, ln))
PY

COUNT="$(wc -l < "$TMP/cands.txt" | tr -d ' ')"
if [ "$COUNT" = "0" ]; then
  echo "  no lambdas of $MIN+ lines left in main() — either the sweep is done or the pattern"
  echo "  this derives from has changed. Both are worth knowing; neither is silence."
  exit 0
fi
echo "  $COUNT candidate(s) of ${MIN}+ lines in main(); one full compile each, so this takes a while"
echo
printf "  %-30s %6s %9s %8s\n" "lambda" "lines" "captures" "per 100"
echo "  ---------------------------------------------------------------"

while read -r LINES NAME AT; do
  # Empty this one lambda's capture list on a COPY. The line is `auto NAME = [&](` possibly with
  # the `[&]` on the following line.
  python3 - "$SRC" "$COPY" "$AT" <<'PY'
import sys
src = open(sys.argv[1]).read().split('\n')
i = int(sys.argv[3]) - 1
if '[&]' in src[i]:
    src[i] = src[i].replace('[&]', '[]', 1)
elif i + 1 < len(src) and '[&]' in src[i + 1]:
    src[i + 1] = src[i + 1].replace('[&]', '[]', 1)
open(sys.argv[2], 'w').write('\n'.join(src))
PY
  PROBE="$(printf '%s' "$CC" \
      | sed 's#-o CMakeFiles/daw_engine.dir/apps/daw_engine_main.cpp.o#-fsyntax-only -ferror-limit=0#' \
      | sed "s#$SRC#$COPY#")"
  ( cd "$BUILD" && eval "$PROBE" ) 2>"$TMP/err.txt"
  CAPS="$(grep -oE "'[A-Za-z_][A-Za-z0-9_]*' cannot be implicitly captured" "$TMP/err.txt" \
          | grep -oE "'[A-Za-z_][A-Za-z0-9_]*'" | tr -d "'" | sort -u | wc -l | tr -d ' ')"
  RATIO="$(python3 -c "print('%.1f' % (100.0 * $CAPS / $LINES))")"
  printf "  %-30s %6s %9s %8s\n" "$NAME" "$LINES" "$CAPS" "$RATIO"
done < "$TMP/cands.txt"

echo
echo "  Sort by CAPTURES, not by lines. Lines move for free — they are copied unchanged — while"
echo "  every capture becomes a struct member, a call-site argument and a re-binding line."
echo
echo "  Every count is a LOWER BOUND: constants need no capture, so a constexpr local the body"
echo "  uses will not appear here and will surface as a compile error after the move."
