#!/usr/bin/env python3
"""Check the AE-P1.2 G2-B implementation step map against the two frozen packets.

WHY THIS EXISTS. The first version of this plan was PROSE, and an independent reviewer found
thirteen defects in it: two frozen records landed by no step, four ordering inversions against the
manifests' own `dependencies` edges, seven tests assigned to a step whose state could not make
them pass, and a coverage claim that was asserted rather than derived. Every one of those is a
question a program can answer, and prose is where they hid.

So the step map is the single source of truth (docs/architecture/tasks/
AE-P1.2-g2b-implementation-steps.json), the prose is GENERATED from it, and this checker refuses
the map when it does not close.

WHAT IT CHECKS
  1. Packet identity: both frozen manifests are at the exact commits and manifest SHA-256 digests
     the map names. A step map checked against a moved packet checks nothing.
  2. Record coverage is a BIJECTION: every record in either manifest is either landed by exactly
     one step or listed in `records_not_landed` with a reason. No record is in both, and no record
     is named that neither manifest contains.
  3. Declared dependency edges are respected: for every record->record edge in either manifest's
     `dependencies`, the depended-on record's step is <= the depending record's step, UNLESS the
     map records a `textual_edges` entry in the reverse direction, which is the map's way of
     saying "these two are mutually referential and therefore share a step". In that case they
     MUST share a step.
  4. Textual edges are respected too: an edge from A to B requires step(B) <= step(A), and the
     quote must appear verbatim in B's or A's frozen statement, so an edge cannot be invented.
  5. Test coverage is a BIJECTION over the union of both manifests' test ids, and every test's
     step is >= the step of every record it is bound to. A test bound to no record fails.
  6. Every test binding names records that exist and are landed (not in records_not_landed).
  7. Steps are numbered 1..N with no gap, and every step that any record or test names exists.

WHAT IT DELIBERATELY DOES NOT CHECK: whether the implementation is correct. This checks that the
PLAN closes. The per-step and completion gates in the map are what check the product.
"""

import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAP_PATH = os.path.join(
    ROOT, "docs", "architecture", "tasks", "AE-P1.2-g2b-implementation-steps.json")

failures = []


def fail(message):
    failures.append(message)


def load_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def sha256_file(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def git_head(worktree):
    return subprocess.run(
        ["git", "-C", worktree, "rev-parse", "HEAD"],
        capture_output=True, text=True, check=False).stdout.strip()


def main():
    step_map = load_json(MAP_PATH)

    # ---- 1. packet identity -------------------------------------------------
    manifests = {}
    for key, filename in (("item15", "AE-P1.2-g2b-item15-manifest.json"),
                          ("item18", "AE-P1.2-g2b-item18-manifest.json")):
        spec = step_map[key]
        # RELATIVE TO THIS REPOSITORY'S PARENT, never absolute — see the map's `worktree_note`.
        # tools/repository_integrity_check.sh refuses committed content holding a user-specific
        # checkout path, and a step map that named one would be a fact about one machine.
        worktree = os.path.normpath(os.path.join(ROOT, spec["worktree"]))
        manifest_path = os.path.join(worktree, "docs", "architecture", "tasks", filename)
        if not os.path.isfile(manifest_path):
            fail(f"{key}: manifest not found at {manifest_path}")
            continue
        head = git_head(worktree)
        if head != spec["commit"]:
            fail(f"{key}: worktree HEAD {head} != pinned {spec['commit']}")
        digest = sha256_file(manifest_path)
        if digest != spec["manifest_sha256"]:
            fail(f"{key}: manifest sha256 {digest} != pinned {spec['manifest_sha256']}")
        manifests[key] = load_json(manifest_path)

    if failures:
        report(step_map, 0, 0)
        return 1

    # ---- gather the frozen populations --------------------------------------
    records = {}       # id -> {'deps': [...], 'statement': str, 'kind': str, 'packet': str}
    for key, manifest in manifests.items():
        for record in manifest["records"]:
            rid = record["id"]
            if rid in records:
                # The same id in both packets: keep both statements so a quote check can match
                # either, and union the dependency edges.
                records[rid]["deps"] = sorted(
                    set(records[rid]["deps"]) | set(record.get("dependencies", [])))
                records[rid]["statement"] += "\n" + record.get("statement", "")
                records[rid]["packet"] += "+" + key
                continue
            records[rid] = {
                "deps": list(record.get("dependencies", [])),
                "statement": record.get("statement", ""),
                "kind": record["kind"],
                "packet": key,
            }

    tests = set()
    for manifest in manifests.values():
        for case in manifest["test_cases"]:
            tests.add(case["id"])

    record_steps = step_map["record_steps"]
    not_landed = step_map["records_not_landed"]
    test_steps = step_map["test_steps"]
    step_numbers = [step["n"] for step in step_map["steps"]]

    # ---- 7. step numbering --------------------------------------------------
    if step_numbers != list(range(1, len(step_numbers) + 1)):
        fail(f"steps are not numbered 1..N without a gap: {step_numbers}")
    valid_steps = set(step_numbers)

    # ---- 2. record coverage bijection ---------------------------------------
    both = sorted(set(record_steps) & set(not_landed))
    if both:
        fail(f"records both landed and excused: {both}")
    unknown = sorted((set(record_steps) | set(not_landed)) - set(records))
    if unknown:
        fail(f"named records that neither manifest contains: {unknown}")
    uncovered = sorted(set(records) - set(record_steps) - set(not_landed))
    if uncovered:
        fail(f"frozen records covered by neither a step nor a reason: {uncovered}")
    for rid, step in record_steps.items():
        if step not in valid_steps:
            fail(f"record {rid} names step {step}, which does not exist")

    # ---- 4. textual edges ---------------------------------------------------
    textual = {}
    for edge in step_map.get("textual_edges", []):
        src, dst, quote = edge["from"], edge["to"], edge["quote"]
        if src not in records or dst not in records:
            fail(f"textual edge {src}->{dst} names a record no manifest contains")
            continue
        haystack = records[src]["statement"] + "\n" + records[dst]["statement"]
        if quote not in haystack:
            fail(f"textual edge {src}->{dst} quotes text absent from both statements: {quote!r}")
        textual.setdefault(src, set()).add(dst)
        if src in record_steps and dst in record_steps:
            if record_steps[dst] > record_steps[src]:
                fail(f"textual edge {src}(step {record_steps[src]}) -> "
                     f"{dst}(step {record_steps[dst]}) points forward")

    # ---- 3. declared dependency edges ---------------------------------------
    for rid, info in records.items():
        if rid not in record_steps:
            continue
        for dep in info["deps"]:
            if dep not in records:
                fail(f"record {rid} declares dependency {dep}, which no manifest contains")
                continue
            if dep in not_landed:
                continue
            if dep not in record_steps:
                fail(f"record {rid} depends on {dep}, which is landed by no step")
                continue
            if record_steps[dep] <= record_steps[rid]:
                continue
            # A forward declared edge is only allowed when the map records the MUTUAL
            # relationship, and then both must share a step.
            if dep in textual and rid in textual[dep]:
                if record_steps[dep] != record_steps[rid]:
                    fail(f"{rid} and {dep} are mutually referential but sit in steps "
                         f"{record_steps[rid]} and {record_steps[dep]}; they must share one")
            else:
                fail(f"ordering inversion: {rid}(step {record_steps[rid]}) depends on "
                     f"{dep}(step {record_steps[dep]}), and no textual edge explains it")

    # ---- 5 and 6. test coverage --------------------------------------------
    unknown_tests = sorted(set(test_steps) - tests)
    if unknown_tests:
        fail(f"step map names tests no manifest contains: {unknown_tests}")
    missing_tests = sorted(tests - set(test_steps))
    if missing_tests:
        fail(f"frozen tests landed by no step: {missing_tests}")
    for tid, binding in test_steps.items():
        step = binding["step"]
        if step not in valid_steps:
            fail(f"test {tid} names step {step}, which does not exist")
        bound = binding.get("records", [])
        if not bound:
            fail(f"test {tid} is bound to no record")
        if not binding.get("why", "").strip():
            fail(f"test {tid} has no stated reason for its binding")
        for rid in bound:
            if rid not in records:
                fail(f"test {tid} binds record {rid}, which no manifest contains")
                continue
            if rid in not_landed:
                fail(f"test {tid} binds {rid}, which is excused as not landed")
                continue
            if rid not in record_steps:
                fail(f"test {tid} binds {rid}, which is landed by no step")
                continue
            if record_steps[rid] > step:
                fail(f"test {tid} is at step {step} but binds {rid} at step {record_steps[rid]}")

    # ---- 8. the prose is GENERATED, never hand-maintained --------------------
    #
    # The convergence protocol's rule, applied to this plan: "Packet prose, summaries, counts,
    # dependency diagrams, and self-checks MUST be generated from or mechanically checked against
    # that manifest. Repeated hand-written counts are forbidden." The first plan's counts were
    # hand-written and one of them was wrong.
    expected = render_markdown(step_map, records, record_steps, test_steps)
    md_path = os.path.join(
        ROOT, "docs", "architecture", "tasks", "AE-P1.2-g2b-implementation-plan.md")
    if "--write" in sys.argv:
        with open(md_path, "w", encoding="utf-8") as handle:
            handle.write(expected)
    else:
        actual = ""
        if os.path.isfile(md_path):
            with open(md_path, "r", encoding="utf-8") as handle:
                actual = handle.read()
        if actual != expected:
            fail(f"{os.path.relpath(md_path, ROOT)} is not the generated prose for this map; "
                 f"re-run with --write")

    report(step_map, len(records), len(tests))
    return 1 if failures else 0


def render_markdown(step_map, records, record_steps, test_steps):
    out = []
    w = out.append
    w("# AE-P1.2 G2-B — combined item-15/item-18 implementation plan")
    w("")
    w(f"> Generated from `{os.path.basename(MAP_PATH)}` by "
      f"`tools/architecture/ae_p1_2_g2b_impl_steps_check.py --write`; do not edit by hand.")
    w("")
    w(f"- Item 15 packet: `{step_map['item15']['commit']}`, manifest SHA-256 "
      f"`{step_map['item15']['manifest_sha256']}`.")
    w(f"- Item 18 packet: `{step_map['item18']['commit']}`, manifest SHA-256 "
      f"`{step_map['item18']['manifest_sha256']}`.")
    w(f"- Frozen product base: `{step_map['frozen_product']['commit']}` "
      f"(tree `{step_map['frozen_product']['tree']}`).")
    w(f"- Implementation branch: `{step_map['branch']}` (this repository).")
    w("")
    w("## This is one atomic change")
    w("")
    w(step_map["atomicity"])
    w("")
    w("## Steps")
    w("")
    for step in step_map["steps"]:
        here = sorted(r for r, n in record_steps.items() if n == step["n"])
        tests_here = sorted(t for t, b in test_steps.items() if b["step"] == step["n"])
        w(f"### Step {step['n']} — {step['title']}")
        w("")
        w(step["detail"])
        w("")
        if "protocol_bump_rationale" in step:
            w(step["protocol_bump_rationale"])
            w("")
        if here:
            w(f"Records closed ({len(here)}): " + ", ".join(f"`{r}`" for r in here))
        else:
            w("Records closed (0): none. This step is PREPARATORY — it builds code a later step "
              "needs in order to close its records intact.")
        w("")
        if tests_here:
            w(f"Tests landed ({len(tests_here)}): " + ", ".join(f"`{t}`" for t in tests_here))
            w("")
    w("## Records this plan does not land, and why")
    w("")
    for rid in sorted(step_map["records_not_landed"]):
        w(f"- `{rid}` — {step_map['records_not_landed'][rid]}")
    w("")
    w("## Mutual references that force records to share a step")
    w("")
    w("Each row is a place where a record's own frozen STATEMENT names a construct introduced by "
      "a record that declares it as a dependency. The declared graph is a DAG; these are the "
      "edges it does not carry, and they are what collapse the dispatch and offline clusters.")
    w("")
    for edge in step_map.get("textual_edges", []):
        note = edge.get("note", "")
        w(f"- `{edge['from']}` (step {record_steps.get(edge['from'], '?')}) needs "
          f"`{edge['to']}` (step {record_steps.get(edge['to'], '?')}): \"{edge['quote']}\"" +
          (f" — {note}" if note else ""))
    w("")
    w("## Test bindings")
    w("")
    w("| test | step | records it needs | why |")
    w("|---|---|---|---|")
    for tid in sorted(test_steps):
        binding = test_steps[tid]
        w(f"| `{tid}` | {binding['step']} | " +
          ", ".join(f"`{r}`" for r in binding["records"]) + f" | {binding['why']} |")
    w("")
    w("## Gate at every step")
    w("")
    for line in step_map["per_step_gate"]:
        w(f"- {line}")
    w("")
    w("## Completion gate for the branch")
    w("")
    for line in step_map["completion_gate"]:
        w(f"- {line}")
    w("")
    return "\n".join(out)


def report(step_map, record_count, test_count):
    if failures:
        print("AE-P1.2 G2-B implementation step map: FAIL")
        for message in failures:
            print(f"  - {message}")
        return
    landed = step_map["record_steps"]
    print("AE-P1.2 G2-B implementation step map: PASS")
    print(f"  steps: {len(step_map['steps'])}")
    print(f"  frozen records: {record_count} "
          f"({len(landed)} landed, {len(step_map['records_not_landed'])} excused)")
    print(f"  frozen tests: {test_count} (all bound)")
    print(f"  textual edges: {len(step_map.get('textual_edges', []))} (all quoted and backward)")
    per_step = {}
    for rid, step in landed.items():
        per_step.setdefault(step, []).append(rid)
    preparatory = 0
    for step in sorted(s["n"] for s in step_map["steps"]):
        here = per_step.get(step, [])
        tests_here = sorted(t for t, b in step_map["test_steps"].items() if b["step"] == step)
        # A step that closes no record is PREPARATORY: it builds code a later step needs in order
        # to close its records intact. That is the `atomicity` statement in action, not an
        # omission — and printing it as zero/zero rather than skipping the row is what stops a
        # reader concluding the step map lost a step.
        label = " (preparatory)" if not here and not tests_here else ""
        if label:
            preparatory += 1
        print(f"  step {step}: {len(here)} records, {len(tests_here)} tests{label}")
    print(f"  preparatory steps: {preparatory} of {len(step_map['steps'])}")


if __name__ == "__main__":
    sys.exit(main())
