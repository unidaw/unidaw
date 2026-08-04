#!/usr/bin/env bash
# REBUILD ONE UNIT TEST OUT OF TREE, so a negative control can run while the full suite is busy.
#
# ctest takes ~25 minutes here. A negative control needs a sabotage, a rebuild, a run and a
# restore — four steps that would either wait for the suite or corrupt it by rebuilding under it.
# This builds the named test's own sources into a scratch directory and links against the JUCE
# objects and archives the real build already produced, leaving build/ untouched.
#
#   usage: tools/nc_unit_build.sh <test-target> [extra-source.cpp ...]
#   e.g.   tools/nc_unit_build.sh engine_sampler_commands_tests apps/engine_sampler_commands.cpp
#
# Prints the path of the binary it built. Exits non-zero if it could not build one.
#
# TWO THINGS HERE ARE LESSONS, NOT STYLE:
#
#   CMake names its objects <source>.cpp.o, not <source>.o. A substitution that assumes the latter
#   silently matches nothing, the stale in-tree object stays in the link, and the control binary
#   is built from unsabotaged code — which prints EXACTLY like a live check that found no problem.
#   That happened; the sabotage was declared blind when it had never been compiled. The script
#   fails loudly now if a source it was asked to replace is not accounted for.
#
#   An object that comes from an ARCHIVE must be listed BEFORE that archive. The linker takes the
#   first definition and only pulls an archive member for a symbol still undefined, so ours wins.
#   If the archive member were pulled anyway there would be a duplicate-symbol error — which makes
#   a successful link the proof that the control reached the binary, rather than an assumption.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
TARGET="${1:-}"
shift || true

if [ -z "$TARGET" ]; then
  echo "usage: tools/nc_unit_build.sh <test-target> [extra-source.cpp ...]" >&2
  exit 2
fi
LINKTXT="$BUILD/CMakeFiles/$TARGET.dir/link.txt"
if [ ! -f "$LINKTXT" ]; then
  echo "nc_unit_build: no link line for target '$TARGET' — is it built? ($LINKTXT)" >&2
  exit 2
fi
if [ ! -f "$BUILD/compile_commands.json" ]; then
  echo "nc_unit_build: build/compile_commands.json is missing; configure with" >&2
  echo "               -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
  exit 2
fi

OUT="${NC_OUT_DIR:-${TMPDIR:-/tmp}/nc_unit/$TARGET}"
mkdir -p "$OUT"

SOURCES="apps/${TARGET}_main.cpp"
for extra in "$@"; do SOURCES="$SOURCES $extra"; done

TARGET="$TARGET" OUT="$OUT" SOURCES="$SOURCES" ROOT="$ROOT" python3 - <<'PY' > "$OUT/build.sh"
import json, os, re, sys

root, out, target = os.environ['ROOT'], os.environ['OUT'], os.environ['TARGET']
sources = os.environ['SOURCES'].split()
cc = json.load(open(os.path.join(root, 'build/compile_commands.json')))
link = open(os.path.join(root, 'build/CMakeFiles/%s.dir/link.txt' % target)).read().strip()

print('set -e')
for src in sources:
    base = os.path.basename(src)
    hits = [e for e in cc if e['file'].endswith('/' + base)]
    if not hits:
        sys.exit('nc_unit_build: %s is not in compile_commands.json' % src)
    obj = '%s/%s.o' % (out, base[:-4])
    print(re.sub(r'-o \S+', '-o ' + obj, hits[0]['command']))
    # <source>.cpp.o is the CMake name. Getting this wrong is why this script exists.
    pat = r'\S*%s\.o\b' % re.escape(base)
    link, n = re.subn(pat, obj, link)
    if n == 0:
        # It comes from an archive, so it must precede every .a in the line (see the header).
        link, n = re.subn(r'(\s)(\S*lib\w+\.a)', r'\1' + obj + r'\1\2', link, count=1)
        if n == 0:
            sys.exit('nc_unit_build: %s is neither an object nor before an archive in the link '
                     'line — the control would not reach the binary' % src)
link = re.sub(r'-o \S+', '-o %s/%s' % (out, target), link)
print('cd %s/build && %s' % (root, link))
PY

bash "$OUT/build.sh"

# ---- A SABOTAGE IN A HEADER NEEDS EVERY TU THAT INSTANTIATES IT REBUILT, and forgetting one prints
# exactly like a check that found no problem. It happened here: the drop accounting lives in a
# template in engine_ui_publish.h, the test calls it through a function in engine_ui_publish.cpp,
# and rebuilding only the test binary left the OLD accounting linked in. The control said PASS.
#
# This cannot know which TUs matter without an include graph, so it says what it does know: which
# headers are modified, and that each of their includers must be on the command line.
if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  DIRTY_H="$(git -C "$ROOT" diff --name-only -- 'apps/*.h' 2>/dev/null || true)"
  if [ -n "$DIRTY_H" ]; then
    echo "nc_unit_build: NOTE — modified header(s) in this tree:" >&2
    printf '%s\n' "$DIRTY_H" | sed 's/^/                 /' >&2
    echo "               If your change is IN one of them, every .cpp that includes it must be" >&2
    echo "               passed as an extra source, or the control will not reach the binary." >&2
  fi
fi

echo "$OUT/$TARGET"
