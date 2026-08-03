#!/usr/bin/env bash
# THE HAZARD POINTER IS seq_cst ON BOTH SIDES, AND THERE IS EXACTLY ONE ACQUIRE.
#
# The track list is reclaimed under a single hazard pointer. The reader publishes its candidate as
# the hazard then re-reads the head; the writer swaps the head then reads the hazard. That is a
# StoreLoad handoff, and StoreLoad is the one ordering that acquire/release does NOT give you —
# both sides must be seq_cst or the store and the load can reorder and reopen the window.
#
# THIS IS NOT THEORETICAL, AND THE REPO HAS ALREADY PAID FOR IT. An earlier version stored the
# hazard with release, re-checked once, and on mismatch reloaded and republished with no final
# re-check. The writer then freed the version between the reader's reload and its hazard store,
# and the audio thread read a freed TrackInfo whose header was null: SIGSEGV at
# header->numChannelsOut (null + 0x1c), a few hundred milliseconds into playback.
#
# WHY A CHECK AND NOT A COMMENT. `std::memory_order_seq_cst` is the strongest and slowest ordering,
# so it is a standing invitation to "optimise" — and a reviewer weakening it sees only a fence,
# not a use-after-free. The comment explaining this already existed, in triplicate, and one of
# those copies argued FOR keeping the duplication while miscounting how many copies there were.
# A comment cannot fail; this can.
#
# THE SECOND RULE, and the reason this is not just a grep for seq_cst: there must be exactly ONE
# acquire loop. Three call sites had it written out verbatim. A fourth hand-written copy is how a
# weakened ordering gets introduced in one place and missed in review of the others.
#
# Pure source analysis; no engine, no audio device.
#   tools/hazard_order_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT" <<'PY'
import re, sys, pathlib
root = pathlib.Path(sys.argv[1])
# READ THE ENGINE'S SOURCES, NOT ONE NAMED FILE. This said 'apps/daw_engine_main.cpp' and went
# red the moment EngineAudioCallback — which owns the hazard pointer — moved to its own header.
# Nothing was wrong; the check had simply lost sight of its subject, and it said so rather than
# passing on an empty search, which is the only reason the move was noticed. A check keyed to a
# path has an expiry date that the next refactor sets. Second one this session (op_registry was
# the first), so the pattern is the lesson rather than the individual fix.
src = '\n'.join(
    p.read_text(errors='ignore')
    for p in sorted((root / 'apps').glob('engine_*.h')) +
             sorted((root / 'apps').glob('engine_*.cpp')) +
             [root / 'apps' / 'daw_engine_main.cpp'])
ok = True

# Comments are stripped first. Every one of these rules is ALSO stated in prose right beside the
# code, so matching raw text would find the description of the invariant and call it satisfied.
body = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
body = re.sub(r'//[^\n]*', ' ', body)

# ---- rule 1: every atomic op on either member is seq_cst.
ops = re.findall(r'(m_tracksPtr|m_tracksHazard)\.(load|store|exchange|compare_exchange\w*)\(([^;]*?)\)\s*;',
                 body)
if not ops:
    print("  FAIL: found no atomic operations on m_tracksPtr / m_tracksHazard at all.")
    print("        Either the members were renamed or this check has stopped looking at the")
    print("        right thing — which is worse than a violation, because it passes.")
    ok = False
weak = [(m, o, a.strip()) for m, o, a in ops if 'seq_cst' not in a]
for m, o, a in weak:
    print("  FAIL: %s.%s(%s) is not seq_cst." % (m, o, a))
    print("        The reader/writer handoff is StoreLoad; acquire/release does not order it.")
    print("        Weakening this reopens the window that produced a SIGSEGV on the audio thread.")
    ok = False

# ---- rule 2: exactly one acquire loop — one place that publishes the hazard.
publishes = len(re.findall(r'm_tracksHazard\.store\(', body))
if publishes != 1:
    print("  FAIL: the hazard is published in %d places; there must be exactly 1 (acquireTracks)."
          % publishes)
    print("        Three call sites once had this loop written out verbatim. A hand-written")
    print("        fourth is how one copy gets weakened while the others are reviewed as fine.")
    ok = False

# ---- rule 3: the acquire is store-then-reload, in that order, and re-checks.
m = re.search(r'std::vector<TrackInfo>\*\s+acquireTracks\(\)\s*\{(.*?)\n  \}', body, re.S)
if not m:
    print("  FAIL: acquireTracks() is gone. The acquire protocol has no single home again.")
    ok = False
else:
    seq = [(a, b) for a, b, _ in
           re.findall(r'(m_tracksPtr|m_tracksHazard)\.(load|store)\(([^)]*)\)', m.group(1))]
    want = [('m_tracksPtr', 'load'), ('m_tracksHazard', 'store'), ('m_tracksPtr', 'load')]
    if seq != want:
        print("  FAIL: acquireTracks does %s; the protocol is %s." % (seq, want))
        print("        Reading the head BEFORE publishing the hazard, or not re-reading it")
        print("        after, is the exact shape that freed a TrackInfo under the audio thread.")
        ok = False

if ok:
    print("  %d atomic op(s) on the track hazard, all seq_cst; one acquire, store-then-recheck"
          % len(ops))
raise SystemExit(0 if ok else 1)
PY
rc=$?
[ "$rc" = "0" ] && echo "hazard_order_check: PASS — the reclamation handoff is fully ordered" \
                || { echo "hazard_order_check: FAIL"; exit 1; }
