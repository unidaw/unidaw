#!/usr/bin/env bash
# WHO WRITES A WHOLE `EventEntry` IN patcher_rust, BECAUSE THAT IS WHAT REACHES THE TAIL.
#
# AE-P1.2 item 35. R14 asks for parity — declare `ready` on the Rust side. The product refuses, and
# the refusal is upheld: `patcher_rust`'s `EventEntry` mirrors six of the C++ type's seven members
# and deliberately omits `ready`, because that buffer has ONE producer and ONE consumer on ONE
# thread, where a publication flag would mean nothing. Declaring it would create a field this crate
# must never write, inside a type whose whole-object store WOULD write it — worse than a documented
# absence.
#
# DISAMBIGUATE BY PATH. There are two Rust types called `EventEntry`. `ui/daw-bridge/src/layout.rs`
# DOES declare `ready` and is not the subject here. The governed one is `patcher_rust/src/lib.rs`.
# Grepping only `ui/` finds the first, sees `ready`, and concludes item 35 is already done — which is
# the second time this type name has produced a wrong answer by measuring the wrong file.
#
# WHAT THE REFUSAL RESTS ON, and the reason this file exists: one sentence of prose in that doc
# comment — *"`push_event` stores the whole object, which writes this type's tail padding over those
# four bytes — harmless, because nothing reads them from this buffer."* Compile-time assertions
# already pin size, alignment and every offset, so the catastrophic stride drift is a build error.
# They cannot see a SECOND whole-object writer appearing, aimed at a buffer where the tail is not
# padding. In a genuine multi-producer ring those four bytes are `ready`, and zeroing them is silent
# data loss on the audio thread.
#
# THE PREDICATE IS THE WRITE WIDTH, NOT THE BUFFER. Two sites take a mutable slice of the same
# buffer and mutate FIELDS of existing entries (`entry.type_`, the payload) — those never touch the
# tail and are not the hazard. Only a WHOLE-OBJECT store covers the padding. So this pins:
#
#   1. exactly one whole-object store `*x = entry;`, and it must be inside `push_event`
#   2. exactly two bulk mutable views of the buffer, so a new bulk-write site earns a look
#
# Both numbers are DERIVED at run time and compared, never typed into a table that drifts.
#
#   tools/patcher_event_tail_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/patcher_rust/src/lib.rs"

[ -f "$SRC" ] || { echo "  FAIL: $SRC not found"; echo "patcher_event_tail_check: FAILED"; exit 1; }

python3 - "$SRC" <<'PYEOF'
import re, sys

path = sys.argv[1]
lines = open(path).read().splitlines()

EXPECTED_WHOLE_STORES = 1
EXPECTED_BULK_VIEWS = 2

# A whole-object store: assigning an `entry` through a dereferenced pointer. This is the only shape
# that writes the type's tail padding, which is where the C++ side keeps `ready`.
# ANYWHERE ON THE LINE, AND EITHER SHAPE. This was first written anchored to the start of a
# line, and its own negative control walked straight past it: a store written inline —
# `unsafe { let slot = ...; *slot = entry; }` — is not at a line start and was invisible. A
# predicate defined by POSITION cannot see the construct that sits somewhere else, which is
# the recurring shape of a check that looks thorough and is not. Index assignment counts too:
# `events[i] = entry;` covers the tail exactly as a deref store does.
STORE = re.compile(r'(?:\*[A-Za-z_][\w.()]*|[A-Za-z_]\w*\[[^\]]*\])\s*=\s*entry\s*;')
BULK = re.compile(r'from_raw_parts_mut\(\s*ctx_ref\.event_buffer')

stores, bulk = [], []
for i, line in enumerate(lines, 1):
    stripped = line.lstrip()
    if stripped.startswith('//') or stripped.startswith('///'):
        continue
    if STORE.search(line):
        stores.append((i, stripped))
    if BULK.search(line):
        bulk.append((i, stripped))

# Which function encloses a line — found by walking back to the nearest `fn` at a shallower indent.
def enclosing_fn(lineno):
    for j in range(lineno - 1, -1, -1):
        m = re.match(r'^\s*(?:pub\s+)?(?:unsafe\s+)?(?:extern\s+"C"\s+)?fn\s+(\w+)', lines[j])
        if m:
            return m.group(1)
    return None

bad = []
if len(stores) != EXPECTED_WHOLE_STORES:
    bad.append("%d whole-object EventEntry store(s), expected exactly %d."
               % (len(stores), EXPECTED_WHOLE_STORES))
    bad.append("  A whole-object store writes the type's TAIL PADDING. In this buffer that is")
    bad.append("  harmless because nothing reads it; in the C++ multi-producer ring those four")
    bad.append("  bytes are `ready`, and clearing them loses the entry silently on the audio")
    bad.append("  thread. A new one has to be read before it is trusted.")
    for n, t in stores:
        bad.append("    lib.rs:%d  %s" % (n, t[:70]))

for n, t in stores:
    fn = enclosing_fn(n)
    if fn != 'push_event':
        bad.append("whole-object store at lib.rs:%d is in `%s`, not `push_event`." % (n, fn))
        bad.append("  The doc comment on EventEntry names push_event as the one place this happens,")
        bad.append("  and the refusal to mirror `ready` is argued from exactly that.")

if len(bulk) != EXPECTED_BULK_VIEWS:
    bad.append("%d bulk mutable view(s) of the event buffer, expected exactly %d."
               % (len(bulk), EXPECTED_BULK_VIEWS))
    bad.append("  The two known ones mutate FIELDS of existing entries and never touch the tail,")
    bad.append("  which is why they are not the hazard. A new one is only safe if it does the")
    bad.append("  same — and that is a claim to check by reading it, not to assume.")
    for n, t in bulk:
        bad.append("    lib.rs:%d  %s" % (n, t[:70]))

if bad:
    print()
    for b in bad:
        print("  FAIL: %s" % b if not b.startswith("  ") else b)
    print()
    raise SystemExit(1)

print("  PASS  %d whole-object store in push_event, %d field-only bulk view(s)"
      % (len(stores), len(bulk)))
for n, _t in stores:
    print("        store   lib.rs:%d" % n)
for n, _t in bulk:
    print("        bulk    lib.rs:%d" % n)
PYEOF
rc=$?
if [ $rc -ne 0 ]; then
  echo "patcher_event_tail_check: FAILED"
  exit 1
fi
echo "patcher_event_tail_check: PASS"
