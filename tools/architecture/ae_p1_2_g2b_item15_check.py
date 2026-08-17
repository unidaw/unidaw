#!/usr/bin/env python3
"""Structural and evidence checker for the focused AE-P1.2 G2-B item-15 packet."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = ROOT / "docs/architecture/tasks/AE-P1.2-g2b-item15-manifest.json"
PROSE_PATH = ROOT / "docs/architecture/tasks/AE-P1.2-g2b-item15.md"
PREDECESSOR_MANIFEST = "docs/architecture/tasks/AE-P1.2-manifest.json"

TOP_LEVEL_KEYS = {
    "schema", "ticket", "status", "owner", "predecessor", "reopening_reason",
    "program_source", "frozen_product", "scope", "implementation_authorized",
    "blocked_by", "non_goals", "governed_files", "changed_records", "records",
    "test_cases",
}
REQUIRED_RECORDS = {
    "G-ITEM15", "DEP-PREDECESSOR", "DEP-FROZEN-BASE", "DEP-ITEM18",
    "E-PROBE-CAUSALITY", "R-CALLER-HELD", "R-AUTHORED-PLAN",
    "R-RESTART-ORDER", "R-FAILURE-WITHDRAWS", "R-NO-AUTHORIZATION",
    "D-DETERMINISTIC-SEAM", "CTRL-PACKET", "CTRL-MUTATIONS",
}
REQUIRED_TESTS = {
    "T-LOCK-CAPABILITY", "T-RT-BEFORE-OFFLINE", "T-EXACT-SLOTS",
    "T-STALE-PLAN", "T-SEND-FAILURE", "T-ORDER-CONTROL",
}


class Refused(RuntimeError):
    pass


def refuse(condition: bool, message: str) -> None:
    if condition:
        raise Refused(message)


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=ROOT)


def object_tree(commit: str) -> str:
    return git("show", "-s", "--format=%T", commit).decode().strip()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_manifest() -> dict:
    return json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))


def source_lines(manifest: dict, locator: str) -> list[str]:
    match = re.fullmatch(r"(frozen|predecessor):([^:]+):(\d+)-(\d+)", locator)
    refuse(match is None, f"unparseable source locator: {locator}")
    authority, path, first_text, last_text = match.groups()
    first, last = int(first_text), int(last_text)
    refuse(first < 1 or last < first, f"invalid source range: {locator}")
    if authority == "frozen":
        data = (ROOT / path).read_text(encoding="utf-8")
    else:
        commit = manifest["predecessor"]["packet_commit"]
        data = git("show", f"{commit}:{path}").decode()
    lines = data.splitlines()
    refuse(last > len(lines), f"source range exceeds file: {locator}")
    return lines[first - 1:last]


def render(manifest: dict) -> str:
    lines = [
        "# AE-P1.2 G2-B — item 15 lock-contract successor",
        "",
        "> Generated from `AE-P1.2-g2b-item15-manifest.json`; do not edit by hand.",
        "",
        f"Status: `{manifest['status']}`. Owner: `{manifest['owner']}`.",
        f"Frozen product: `{manifest['frozen_product']['commit']}` (tree `{manifest['frozen_product']['tree']}`).",
        f"Program source: `{manifest['program_source']['commit']}` (tree `{manifest['program_source']['tree']}`).",
        f"Successor to packet `{manifest['predecessor']['packet_commit']}` / manifest `{manifest['predecessor']['manifest_sha256']}`.",
        f"Reopening reason: {manifest['reopening_reason']}",
        "",
        "## Scope", "", manifest["scope"], "",
        f"Implementation authorized: `{str(manifest['implementation_authorized']).lower()}`.",
        "", "## Blocked by", "",
    ]
    lines.extend(f"- {item}" for item in manifest["blocked_by"])
    lines.extend(["", "## Records", ""])
    for record in manifest["records"]:
        lines.append(
            f"- `{record['id']}` [{record['kind']} / {record['status']}]: {record['statement']}"
        )
    lines.extend(["", "## Required future test cases", ""])
    for case in manifest["test_cases"]:
        lines.append(f"- `{case['id']}`: {case['statement']}")
    lines.extend(["", "## Non-goals", ""])
    lines.extend(f"- {item}" for item in manifest["non_goals"])
    lines.extend([
        "", "## Review requirement", "",
        "This packet may close item 15's planning choice only after independent semantic and evidence reviewers both return PASS for the same immutable packet SHA and frozen product base. Any product implementation requires a separate reviewed successor and implementation ticket after the two blockers above are resolved.",
        "",
    ])
    return "\n".join(lines)


def validate(manifest: dict, *, verify_files: bool = True, verify_prose: bool = True) -> None:
    refuse(set(manifest) != TOP_LEVEL_KEYS, "top-level manifest shape changed")
    refuse(manifest["schema"] != "ae-p1.2-g2b-item15-packet/1", "schema changed")
    refuse(manifest["ticket"] != "AE-P1.2-G2B-ITEM15", "ticket changed")
    refuse(manifest["status"] != "REVIEW_CANDIDATE", "packet is not a review candidate")
    refuse(manifest["implementation_authorized"] is not False,
           "item 15 packet must not authorize implementation")
    refuse(len(manifest["blocked_by"]) != 2, "implementation blockers changed")

    records = manifest["records"]
    ids = [record.get("id") for record in records]
    refuse(len(ids) != len(set(ids)), "duplicate record id")
    refuse(set(ids) != REQUIRED_RECORDS, "record set changed")
    refuse(manifest["changed_records"] != sorted(REQUIRED_RECORDS),
           "changed_records is not the exact sorted record set")

    by_id = {record["id"]: record for record in records}
    for record in records:
        refuse(set(record) != {"id", "kind", "owner", "status", "dependencies",
                               "source_span", "control", "statement"},
               f"record shape changed: {record.get('id')}")
        refuse(record["owner"] != "backend", f"unexpected owner: {record['id']}")
        refuse(record["control"] not in REQUIRED_RECORDS,
               f"unknown control: {record['id']}")
        for dep in record["dependencies"]:
            refuse(dep not in by_id, f"unknown dependency {dep} from {record['id']}")
        spans = record["source_span"] if isinstance(record["source_span"], list) else [record["source_span"]]
        for span in spans:
            if span.startswith("manifest:/") or span.startswith("packet:"):
                continue
            source_lines(manifest, span)

    visiting: set[str] = set()
    visited: set[str] = set()
    def walk(record_id: str) -> None:
        refuse(record_id in visiting, f"dependency cycle at {record_id}")
        if record_id in visited:
            return
        visiting.add(record_id)
        for dep in by_id[record_id]["dependencies"]:
            walk(dep)
        visiting.remove(record_id)
        visited.add(record_id)
    walk("G-ITEM15")
    refuse(visited != REQUIRED_RECORDS, "gate dependency closure is incomplete")

    test_ids = [case.get("id") for case in manifest["test_cases"]]
    refuse(len(test_ids) != len(set(test_ids)), "duplicate test id")
    refuse(set(test_ids) != REQUIRED_TESTS, "test case set changed")
    failure_case = next(case for case in manifest["test_cases"] if case["id"] == "T-SEND-FAILURE")
    refuse("stream unusable until disconnect/relaunch" not in failure_case["statement"],
           "partial-frame transport poison is not tested")

    caller = by_id["R-CALLER-HELD"]["statement"]
    refuse("unique_lock" not in caller or "never locks controllerMutex internally" not in caller,
           "caller-held lock contract weakened")
    refuse("partial SOCK_STREAM frame" not in by_id["R-FAILURE-WITHDRAWS"]["statement"],
           "partial frame failure semantics omitted")
    refuse("does not authorize" not in by_id["G-ITEM15"]["statement"],
           "gate overstates authorization")

    if verify_files:
        frozen = manifest["frozen_product"]
        refuse(object_tree(frozen["commit"]) != frozen["tree"], "frozen product tree mismatch")
        program = manifest["program_source"]
        refuse(object_tree(program["commit"]) != program["tree"], "program source tree mismatch")
        predecessor = manifest["predecessor"]
        refuse(object_tree(predecessor["packet_commit"]) != predecessor["packet_tree"],
               "predecessor packet tree mismatch")
        predecessor_bytes = git("show", f"{predecessor['packet_commit']}:{PREDECESSOR_MANIFEST}")
        refuse(sha256(predecessor_bytes) != predecessor["manifest_sha256"],
               "predecessor manifest digest mismatch")

        paths = [entry["path"] for entry in manifest["governed_files"]]
        refuse(paths != sorted(paths) or len(paths) != len(set(paths)),
               "governed file paths must be sorted and unique")
        for entry in manifest["governed_files"]:
            path = ROOT / entry["path"]
            refuse(not path.is_file(), f"missing governed file: {entry['path']}")
            refuse(sha256(path.read_bytes()) != entry["sha256"],
                   f"governed file drift: {entry['path']}")

        produce = (ROOT / "apps/engine_produce_block.cpp").read_text(encoding="utf-8")
        refuse(not re.search(r"if \(!sentOk\).*?hostReady\.store\(false", produce, re.S),
               "ProcessBlock failure no longer proves hostReady withdrawal")
        apply = (ROOT / "apps/daw_engine_main.cpp").read_text(encoding="utf-8")
        refuse(not re.search(r"applyHostBypassStates.*?if \(!runtime\.hostReady.*?return;.*?controllerMutex", apply, re.S),
               "bypass guard/lock causal path changed")
        probe = (ROOT / "tools/bypass_send_probe.sh").read_text(encoding="utf-8")
        refuse(not re.search(r"do play.*?kill -STOP.*?sleep 5.*?do set-bypass", probe, re.S),
               "probe no longer fills ProcessBlock traffic before bypass")

    if verify_prose:
        refuse(PROSE_PATH.read_text(encoding="utf-8") != render(manifest),
               "generated prose differs from the manifest")


def self_test(manifest: dict) -> None:
    cases = []

    missing_lock = copy.deepcopy(manifest)
    missing_lock["records"] = [r for r in missing_lock["records"] if r["id"] != "R-CALLER-HELD"]
    cases.append(("missing caller-held ruling", missing_lock))

    authorized = copy.deepcopy(manifest)
    authorized["implementation_authorized"] = True
    cases.append(("false implementation authorization", authorized))

    no_poison = copy.deepcopy(manifest)
    next(c for c in no_poison["test_cases"] if c["id"] == "T-SEND-FAILURE")["statement"] = \
        "A failed send is reported."
    cases.append(("partial-frame case omitted", no_poison))

    broken_dep = copy.deepcopy(manifest)
    next(r for r in broken_dep["records"] if r["id"] == "G-ITEM15")["dependencies"].append("NO-SUCH-RECORD")
    cases.append(("broken dependency", broken_dep))

    for name, candidate in cases:
        try:
            validate(candidate, verify_files=False, verify_prose=False)
        except Refused:
            continue
        raise Refused(f"mutation was accepted: {name}")


def main() -> int:
    manifest = load_manifest()
    if "--render" in sys.argv:
        sys.stdout.write(render(manifest))
        return 0
    validate(manifest)
    self_test(manifest)
    print("AE-P1.2 G2-B item 15 packet: PASS")
    print(f"  records: {len(manifest['records'])}")
    print(f"  governed files: {len(manifest['governed_files'])}")
    print(f"  mutation controls: 4/4 refused")
    print("  implementation authorized: false")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Refused, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"AE-P1.2 G2-B item 15 packet: REFUSED: {exc}", file=sys.stderr)
        raise SystemExit(1)
