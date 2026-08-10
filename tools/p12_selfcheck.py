#!/usr/bin/env python3
# A.0 for AE-P1.2: every claim the packet makes ABOUT ITSELF, checked.
# Run: python3 p12check.py <packet.md> <pinned-checkout>
import re, subprocess, sys
pkt, pin = open(sys.argv[1]).read(), sys.argv[2]
fail = []
def bad(t, d): fail.append(f'[{t}] {d}')

# 1. INTERNAL CONSISTENCY: a summary count must equal what the body contains.
m = re.search(r'# Open items — (\d+), atomic', pkt)
body = re.search(r'# Open items.*?(?=# Provenance)', pkt, re.S).group(0)
# A real item marker CONTINUES the sequence. Any "N. " in prose ("a list of 15. 8. A region")
# matches a bare number regex, so accept a candidate only if it equals the last accepted + 1.
cand = [int(n) for n in re.findall(r'(?<![\d.\w])(\d{1,2})\.\s', re.sub(r'\s+', ' ', body))]
nums, nxt = [], 1
for c in cand:
    if c == nxt: nums.append(c); nxt += 1
if not m: bad('OPEN-HEADER-MISSING', 'no "# Open items — N, atomic"')
elif int(m.group(1)) != len(nums): bad('OPEN-COUNT', f'header says {m.group(1)}, body has {len(nums)}')
if nums and nums != list(range(1, len(nums) + 1)): bad('OPEN-NOT-CONTIGUOUS', str(nums))
leftover = [c for c in cand if c > len(nums)]
if leftover: bad('OPEN-ORPHAN-NUMBER', f'markers past the sequence: {sorted(set(leftover))}')

# 2. Any earlier statement of the open count must agree with the header.
for n in set(re.findall(r'open list is (\d+) atomic|Open items are (\d+)|(\d+) atomic items', pkt)):
    for v in n:
        if v and m and v != m.group(1): bad('OPEN-COUNT-RESTATED', f'{v} elsewhere vs header {m.group(1)}')

# 3. A withdrawn bullet must not be described as covered anywhere.
if 'WITHDRAWN AS CIRCULAR' in pkt:
    for phrase in ['mirror covered', 'covers mirror replay', 'Mirror replay is staged and acknowledged']:
        if phrase in pkt: bad('WITHDRAWN-STILL-CLAIMED', phrase)

# 4. EVERY PASS bullet carries a refutation or is explicitly withdrawn.
for gid, gbody in zip(*[re.split(r'\n# (G[0-9A-B-]+) — ', pkt)[1:][i::2] for i in (0, 1)]):
    mm = re.search(r'\*\*PASS conditions\.\*\*(.*?)\*\*Static checks', gbody, re.S)
    if not mm: bad('NO-PASS-BLOCK', gid); continue
    for i, b in enumerate(re.findall(r'(?m)^\d+\. (.*?)(?=\n\d+\. |\Z)', mm.group(1), re.S), 1):
        if 'REFUTED BY' not in b and 'WITHDRAWN' not in b: bad('NO-REFUTATION', f'{gid} PASS {i}')

# 5. EVERY RAW count must equal what its command actually returns, run against the pin.
#    Form: RAW <n> (`<command>`)
for n, cmd in re.findall(r'RAW \*{0,2}(\d+)\*{0,2}\s*\(`([^`]+)`\)', pkt):
    # the packet is markdown: '\\->' and '\\.' are escapes for the reader, not the shell
    real = cmd.replace('\\->', '->').replace('\\.', '.').replace('\\[', '[').replace('\\|', '|')
    # NEVER pipe into wc -l before checking status: a missing tool exits non-zero, prints nothing,
    # and wc turns that into the number 0 — indistinguishable from a genuine zero-match count.
    # rg is NOT on the PATH of a bare /bin/sh, which is what a reviewer's checker runs in.
    probe = subprocess.run(real, shell=True, cwd=pin, capture_output=True, text=True, timeout=60)
    if probe.returncode not in (0, 1) or 'command not found' in probe.stderr:
        bad('COMMAND-UNRUNNABLE', f'{probe.stderr.strip()[:50]} :: {real[:60]}'); continue
    got = len([l for l in probe.stdout.splitlines() if l.strip()])
    if got != int(n): bad('RAW-MISMATCH', f'claims {n}, command returns {got} :: {real[:70]}')

print(f'open items {len(nums)}, RAW claims {len(re.findall(r"RAW \*{0,2}\d", pkt))}')
if fail:
    for f in fail: print('  ' + f)
    print(f'FAIL ({len(fail)})'); sys.exit(1)
print('PASS')
