#!/usr/bin/env bash
# EVERY REQUEST KIND HAS A SENDER AND AN ANSWER, OR SAYS WHY IT DOES NOT.
#
# AE-P1.2 G1-B, ruling R1. A request/answer reader is one that reads the region a `Request*` command
# publishes — and that population was authored wrong THREE TIMES before anyone enumerated it from
# the right place:
#
#   a hand-picked six          irreproducible; nobody could state the rule that produced it
#   "carries a requestSeq"     CIRCULAR — it selects on the correlation token, so a reader MISSING
#                              its token, which is the defect the gate exists to find, is invisible
#   "has a send_*_request"     blind where no helper exists: RequestDeviceParams is issued as a bare
#                              command from three crates and has no helper at all
#
# THE FIX WAS TO ENUMERATE FROM THE ARTIFACT THAT CANNOT BE ABSENT. A request kind is a numbered
# member of the `UiCommandType` enum. A token can be missing, a helper can be missing, a name can be
# spelt differently — an enum member is present or the build fails. So the enum is the authority
# here, and the two derived populations are checked AGAINST it rather than gathered alongside it.
#
# THREE POPULATIONS, ENUMERATED SEPARATELY AND THEN JOINED. Conflating any two of them is what
# produced each wrong answer above:
#
#   1. KINDS    the Request* members of UiCommandType, in C++, mirrored in Rust
#   2. SENDERS  where each kind is ISSUED, across every ui/ crate, excluding test code
#   3. READERS  the read_* that consumes each kind's answer — or a DECLARED reason there is none
#
# A kind whose answer does not land in a mapped region is DECLARED below with its reason, not
# omitted. "There is no reader" is a fact about the design; an empty slot is a fact about the
# checker, and the two must not look the same.
#
# Pure source analysis; no engine, no audio device, no build.
#   tools/request_registry_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT" <<'PY'
import re, sys, pathlib

root = pathlib.Path(sys.argv[1])
fail = []
def bad(msg): fail.append(msg)
def note(msg): print("  " + msg)

# ---------------------------------------------------------------- 1. KINDS, from the C++ enum.
cpp = (root / "apps/event_payloads.h").read_text()
m = re.search(r'enum class UiCommandType[^{]*\{(.*?)\n\};', cpp, re.S)
if not m:
    print("FAIL: could not find enum class UiCommandType in apps/event_payloads.h"); sys.exit(1)
KINDS = {name: int(val) for name, val in re.findall(r'^\s*(Request\w+)\s*=\s*(\d+)\s*,', m.group(1), re.M)}

# A floor on the authority itself. Every assertion below is "for each kind", so an extraction that
# finds nothing satisfies all of them. Seven existed when this was written; FEWER means the enum
# moved or the pattern broke, not that the product shrank.
if len(KINDS) < 7:
    bad(f"found {len(KINDS)} Request* kinds in UiCommandType; 7 existed when this was written. "
        "Fewer means the extraction broke — fix the pattern, do not lower this number.")
note(f"{len(KINDS)} request kinds in the C++ enum: " + ", ".join(sorted(KINDS)))

# ------------------------------------------------- 1b. the Rust mirror carries the SAME numbers.
rust_layout = (root / "ui/daw-bridge/src/layout.rs").read_text()
mr = re.search(r'enum UiCommandType[^{]*\{(.*?)\n\}', rust_layout, re.S)
RUST = {name: int(val) for name, val in re.findall(r'^\s*(Request\w+)\s*=\s*(\d+)\s*,', mr.group(1) if mr else "", re.M)}
if RUST != KINDS:
    only_cpp = {k: v for k, v in KINDS.items() if RUST.get(k) != v}
    only_rs  = {k: v for k, v in RUST.items() if KINDS.get(k) != v}
    bad(f"the Rust mirror disagrees with the C++ enum. C++-only/differing: {only_cpp or '{}'}; "
        f"Rust-only/differing: {only_rs or '{}'}. A request kind that means two numbers is a "
        "command sent to one engine and understood by another.")
else:
    note(f"the Rust mirror agrees on all {len(RUST)} kinds and their values")

# --------------------------------------------------- 2. SENDERS, per kind, across every ui/ crate.
# Test code is excluded by MODULE BOUNDARY, not by looking for the word `assert` — a test that
# issues a request without asserting would otherwise be counted as production, and a production
# site that happens to assert would be dropped.
CRATES = ["ui/daw-cli/src/main.rs", "ui/daw-sidecar/src/main.rs", "ui/daw-agent/src/tools.rs"]
senders, excluded = {k: [] for k in KINDS}, 0
for rel in CRATES:
    p = root / rel
    if not p.exists():
        bad(f"{rel} does not exist; the sender census cannot be complete without it."); continue
    lines = p.read_text().split("\n")
    tstart = next((i for i, l in enumerate(lines, 1) if l.strip() == "#[cfg(test)]"), len(lines) + 1)
    for i, l in enumerate(lines, 1):
        for k in KINDS:
            if re.search(r'UiCommandType::' + k + r'\b', l):
                if i >= tstart: excluded += 1
                else: senders[k].append(f"{rel}:{i}")

note(f"{sum(len(v) for v in senders.values())} production send sites "
     f"({excluded} in test modules, excluded by module boundary)")
for k in sorted(KINDS):
    if not senders[k]:
        bad(f"{k} has NO production send site. A request kind nothing issues is either dead or "
            "issued somewhere this census cannot see; both need a person.")

# ---------------------------------------------------------- 3. READERS, declared per kind.
# The answer to a request lands in a mapped region and is read by a named accessor — EXCEPT where
# it does not, and that exception is declared here with its reason rather than left as a gap.
READERS = {
    "RequestClipWindow":      "read_clip_window",
    "RequestDeviceParams":    "read_device_params",
    "RequestWaveform":        "read_waveform_slot",
    "RequestAutomationLane":  "read_automation_slot",
    "RequestSamplerKit":      "read_sampler_kit_slot",
    "RequestSamplerEnvelope": "read_sampler_envelope_slot",
}
NO_REGION_READER = {
    "RequestChainSnapshot":
        "answers into the UI-OUT DIFF RING via emitChainSnapshot (apps/engine_chain_host.cpp), not "
        "into a mapped region; consumed by drain_ui_out. This is the member that proves the rule "
        "does work rather than counting to a target — a rule that admitted it would be enumerating "
        "requests, not finding readers.",
}
control = (root / "ui/daw-bridge/src/control.rs").read_text()

undeclared = sorted(set(KINDS) - set(READERS) - set(NO_REGION_READER))
if undeclared:
    bad(f"{len(undeclared)} request kind(s) are neither mapped to a reader nor declared as having "
        f"none: {undeclared}. Add the mapping, or declare the reason — an undeclared kind is the "
        "shape every wrong version of this population had.")
stale = sorted((set(READERS) | set(NO_REGION_READER)) - set(KINDS))
if stale:
    bad(f"{len(stale)} declared kind(s) no longer exist in the enum: {stale}. The declaration "
        "outlived its subject.")

for kind, fn in sorted(READERS.items()):
    if kind not in KINDS: continue
    if not re.search(r'pub fn ' + fn + r'\s*\(', control):
        bad(f"{kind} is mapped to {fn}(), which does not exist in ui/daw-bridge/src/control.rs. "
            "Either the reader was renamed and this mapping was not, or the answer moved.")
note(f"{len(READERS)} kinds map to a named reader; {len(NO_REGION_READER)} declared as having none")

# The declared exception must STAY true: if a chain reader appears, the declaration is stale and
# the check must say so rather than keep asserting an absence that has ended.
if re.search(r'pub fn read_chain\w*\s*\(', control):
    bad("a chain-snapshot reader now exists in control.rs, but RequestChainSnapshot is still "
        "declared as having no region reader. The declaration is stale.")

for line in fail:
    print("  FAIL  " + line)
print("request_registry_check: " + ("FAILED" if fail else "PASS"))
sys.exit(1 if fail else 0)
PY
