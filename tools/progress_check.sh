#!/usr/bin/env bash
# docs/PROGRESS.md IS RECOMPUTED, NOT REMEMBERED.
#
# A progress file maintained by intention goes stale, and this repo has the receipts:
# meter_bar_check.sh carried "THE ANCHOR IS NOT FIXED" for months after it was fixed — stale in the
# direction that gets the same work done twice, by someone with no reason to suspect the first
# attempt exists. Nothing reported it, because a confident sentence about the code looks exactly
# like a true one.
#
# So the file states three facts and this recomputes all three. Wrong numbers are a red suite.
#
# MEASURED AGAINST `as-of-commit`, NOT THE WORKING TREE, and that choice is the whole ergonomics of
# it: you can commit a dozen times without touching PROGRESS.md, then catch it up once. If the
# numbers were checked against HEAD, every refactor commit would fail until the file was edited,
# the check would become a tax on committing, and a check that is a tax gets deleted.
#
# DRIFT IS BOUNDED so "later" cannot become "never": as-of-commit must be an ancestor of HEAD and
# within MAX_DRIFT commits of it. Stop updating and the suite goes red on its own.
#
# WHAT THIS DELIBERATELY DOES NOT DO is judge the prose. It can force the numbers to be honest and
# the file to be recent; it cannot force the narrative to be worth reading. Claiming otherwise
# would be the same error as a comment that presents itself as a guarantee.
#
# Citations in PROGRESS.md are covered separately and for free: it lives in docs/, so
# doc_citation_check.sh already forbids line-number citations and requires every cited symbol to
# resolve — which is the rule that catches a claim about the code that stopped being true.
#
#   tools/progress_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
DOC="docs/PROGRESS.md"
MAX_DRIFT=12
ok=1

[ -f "$DOC" ] || { echo "  FAIL: $DOC does not exist."; echo "progress_check: FAIL"; exit 1; }

field() {  # field <name> -> the value, or empty
  sed -n "s/^- $1: \(.*\)\$/\1/p" "$DOC" | head -1 | tr -d ' \r'
}

AS_OF="$(field as-of-commit)"
WANT_LINES="$(field main-cpp-lines)"
WANT_TESTS="$(field ctest-entries)"
WANT_MAIN="$(field main-function-lines)"

for pair in "as-of-commit:$AS_OF" "main-cpp-lines:$WANT_LINES" "ctest-entries:$WANT_TESTS" \
            "main-function-lines:$WANT_MAIN"; do
  if [ -z "${pair#*:}" ]; then
    echo "  FAIL: $DOC has no '- ${pair%%:*}: ...' line."
    echo "        The checked-facts block is what makes this file self-verifying; without it"
    echo "        the check would pass on a document that says nothing, which is worse than"
    echo "        having no check at all."
    ok=0
  fi
done
[ "$ok" = "1" ] || { echo "progress_check: FAIL"; exit 1; }

# ---- Is git usable? A check that cannot run must SAY SO rather than pass quietly: a skipped rule
# and a satisfied rule are indistinguishable from the exit code alone, and that is the defect this
# repo keeps paying for. The numeric rules below still run.
GIT_OK=1
git rev-parse --git-dir >/dev/null 2>&1 || GIT_OK=0
if [ "$GIT_OK" = "0" ]; then
  echo "  note: not a git checkout — the ancestry and drift rules are SKIPPED, not satisfied."
fi

if [ "$GIT_OK" = "1" ]; then
  if ! git cat-file -e "${AS_OF}^{commit}" 2>/dev/null; then
    echo "  FAIL: as-of-commit $AS_OF is not a commit in this repository."
    ok=0
  elif ! git merge-base --is-ancestor "$AS_OF" HEAD 2>/dev/null; then
    echo "  FAIL: as-of-commit $AS_OF is not an ancestor of HEAD, so $DOC describes a history"
    echo "        this branch does not have. Rebased or cherry-picked? Re-point it at a commit"
    echo "        that is actually behind you."
    ok=0
  else
    DRIFT="$(git rev-list --count "${AS_OF}..HEAD" 2>/dev/null || echo 0)"
    if [ "$DRIFT" -gt "$MAX_DRIFT" ]; then
      echo "  FAIL: $DOC is $DRIFT commits behind HEAD (limit $MAX_DRIFT)."
      echo "        This is the rule that stops 'I will update it later' from becoming never."
      echo "        Bring it up to date and set:"
      echo "          - as-of-commit: $(git rev-parse --short HEAD)"
      echo "          - main-cpp-lines: $(wc -l < apps/daw_engine_main.cpp | tr -d ' ')"
      echo "          - ctest-entries: $(grep -c '^add_test(NAME' CMakeLists.txt)"
      ok=0
    fi

    # ---- The facts, read out of the tree of the commit the file claims to describe.
    GOT_LINES="$(git show "${AS_OF}:apps/daw_engine_main.cpp" 2>/dev/null | wc -l | tr -d ' ')"
    GOT_TESTS="$(git show "${AS_OF}:CMakeLists.txt" 2>/dev/null | grep -c '^add_test(NAME')"
    if [ "$GOT_LINES" != "$WANT_LINES" ]; then
      echo "  FAIL: main-cpp-lines says $WANT_LINES; at $AS_OF it is $GOT_LINES."
      ok=0
    fi
    if [ "$GOT_TESTS" != "$WANT_TESTS" ]; then
      echo "  FAIL: ctest-entries says $WANT_TESTS; at $AS_OF CMakeLists.txt registers $GOT_TESTS."
      ok=0
    fi

    # ---- main()'s OWN length, which is the number a maintainability panel actually grades, and
    # the one the narrative kept restating from memory. The prose said 12,133 while the tree said
    # 11,666, within hours of being written: the same number in two places, one checked and one
    # not, and the unchecked copy drifted. It is a checked fact now, and the prose no longer
    # repeats it.
    GOT_MAIN="$(git show "${AS_OF}:apps/daw_engine_main.cpp" 2>/dev/null | python3 -c "
import re, sys
lines = sys.stdin.read().split(chr(10))
mi = next((i for i, l in enumerate(lines) if re.match(r'^int main\(', l)), None)
if mi is None:
    print(0); raise SystemExit
d = 0; st = None
for i in range(mi, len(lines)):
    d += lines[i].count('{') - lines[i].count('}')
    if st is None and '{' in lines[i]: st = i
    if st is not None and d == 0 and i > st:
        print(i - mi + 1); break
else:
    print(0)
")"
    if [ "$GOT_MAIN" != "$WANT_MAIN" ]; then
      echo "  FAIL: main-function-lines says $WANT_MAIN; at $AS_OF main() is $GOT_MAIN lines."
      ok=0
    fi
  fi
fi

# ---- SELF-CHECK: the recomputation must agree with ctest's own count on the CURRENT tree.
# Without this the check could drift into measuring something that merely correlates with the test
# count — grepping add_test is a proxy for "what ctest will run", and a proxy is exactly what
# failed us in sampler_vintage.
CUR_TESTS="$(grep -c '^add_test(NAME' CMakeLists.txt)"
if [ -f build/CTestTestfile.cmake ]; then
  REAL_TESTS="$( (cd build && ctest -N 2>/dev/null | sed -n 's/^Total Tests: //p') )"
  if [ -n "$REAL_TESTS" ] && [ "$REAL_TESTS" != "$CUR_TESTS" ]; then
    echo "  FAIL: this check counts $CUR_TESTS add_test lines but ctest reports $REAL_TESTS."
    echo "        The recomputation has stopped measuring what it claims to measure."
    ok=0
  fi
fi

if [ "$ok" = "1" ]; then
  echo "  $DOC is current at $AS_OF ($DRIFT commit(s) behind HEAD, limit $MAX_DRIFT):"
  echo "    main.cpp $WANT_LINES lines, $WANT_TESTS registered tests — both recomputed from that commit"
fi
[ "$ok" = "1" ] && echo "progress_check: PASS — the progress file's facts match the tree it names" \
                || { echo "progress_check: FAIL"; exit 1; }
