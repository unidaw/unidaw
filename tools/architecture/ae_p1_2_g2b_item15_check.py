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
    "revision_predecessor", "program_source", "frozen_product", "scope", "implementation_authorized",
    "blocked_by", "non_goals", "governed_files", "changed_records", "records",
    "review_history", "test_cases",
}
REQUIRED_RECORDS = {
    "G-ITEM15", "DEP-PREDECESSOR", "DEP-FROZEN-BASE", "DEP-ITEM16", "DEP-ITEM18",
    "P-GOVERNED-FILES", "E-PROBE-CONFOUNDER", "R-CALLER-HELD", "R-AUTHORED-PLAN",
    "R-SUPERSEDE-PASS3", "R-RESTART-ORDER", "R-FAILURE-WITHDRAWS", "R-NO-AUTHORIZATION",
    "D-DETERMINISTIC-SEAM", "CTRL-PACKET", "CTRL-MUTATIONS",
}
REQUIRED_TESTS = {
    "T-LOCK-CAPABILITY", "T-RT-BEFORE-OFFLINE", "T-EXACT-SLOTS",
    "T-STALE-PLAN", "T-SEND-FAILURE", "T-ORDER-CONTROL", "T-OLD-GUARD-CONTROL",
}
EXPECTED_FROZEN = {
    "commit": "92dfdfe23cc7ff93f2ce14894a35d089e3d9e2b8",
    "tree": "238ac970b5d61fe16055ede4c43a2978ddb11da7",
}
EXPECTED_PROGRAM = {
    "commit": "02e984f578d1e08ff0773c354ce87aa7826f7f06",
    "tree": "b7965c847d40ec8e4ef5b19359782ac28e49e4c7",
}
EXPECTED_PREDECESSOR = {
    "packet_commit": "2b5f0747f1b7dde79ae788af3826c49c78df5d2a",
    "packet_tree": "7c75beb7b941c06a6099292fbf4dac6ade503a6a",
    "manifest_sha256": "c321130b860fda73991f04d1035bea7af03faf6e030fce5565664c1657ce093e",
}
EXPECTED_REVISION_PREDECESSOR = {
    "packet_commit": "1f86e0015f83c666bb2f925eaeb6105a6011b622",
    "packet_tree": "0ab82ede010e3681ac4e17e97c5dd6c7e4acc39e",
    "manifest_sha256": "0c37a0f222d27292218c26e00cdaa0f34884d270105bed109b038073c13317c4",
}
EXPECTED_GOVERNED_PATHS = (
    "apps/daw_engine_main.cpp",
    "apps/engine_chain_host.cpp",
    "apps/engine_chain_host.h",
    "apps/engine_produce_block.cpp",
    "apps/engine_readiness_tests_main.cpp",
    "apps/engine_restart_worker.cpp",
    "apps/engine_restart_worker.h",
    "apps/host_controller.cpp",
    "apps/ipc_io.cpp",
    "tools/bypass_send_probe.sh",
)
EXPECTED_SOURCE_SPANS = {
    "G-ITEM15": [
        "predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:965-984",
        "predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:2553-2565",
    ],
    "DEP-PREDECESSOR": "predecessor:docs/architecture/tasks/AE-P1.2-manifest.json:1-40",
    "DEP-FROZEN-BASE": "frozen:apps/daw_engine_main.cpp:1058-1077",
    "P-GOVERNED-FILES": "manifest:/governed_files",
    "DEP-ITEM16": "predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:2556-2558",
    "DEP-ITEM18": "predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:2559-2565",
    "E-PROBE-CONFOUNDER": [
        "frozen:tools/bypass_send_probe.sh:2-17",
        "frozen:tools/bypass_send_probe.sh:92-108",
        "frozen:apps/engine_produce_block.cpp:1072-1100",
        "frozen:apps/daw_engine_main.cpp:1058-1061",
    ],
    "R-CALLER-HELD": [
        "frozen:apps/daw_engine_main.cpp:1058-1077",
        "frozen:apps/engine_restart_worker.cpp:102-144",
    ],
    "R-AUTHORED-PLAN": [
        "frozen:apps/daw_engine_main.cpp:1062-1068",
        "frozen:apps/engine_chain_host.cpp:142-272",
    ],
    "R-SUPERSEDE-PASS3": "predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:965-984",
    "R-RESTART-ORDER": "frozen:apps/engine_restart_worker.cpp:102-165",
    "R-FAILURE-WITHDRAWS": [
        "frozen:apps/host_controller.cpp:624-654",
        "frozen:apps/ipc_io.cpp:89-155",
    ],
    "R-NO-AUTHORIZATION": "manifest:/implementation_authorized",
    "D-DETERMINISTIC-SEAM": "manifest:/test_cases",
    "CTRL-PACKET": "packet:tools/architecture/ae_p1_2_g2b_item15_check.py",
    "CTRL-MUTATIONS": "packet:tools/architecture/ae_p1_2_g2b_item15_check.py",
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


def safe_repo_path(path_text: str, authority: str) -> str:
    refuse(not path_text or "\\" in path_text, f"unsafe {authority} path: {path_text}")
    relative = Path(path_text)
    refuse(relative.is_absolute() or ".." in relative.parts or "." in relative.parts,
           f"unsafe {authority} path: {path_text}")
    canonical = relative.as_posix()
    refuse(canonical != path_text, f"non-canonical {authority} path: {path_text}")
    return canonical


def source_lines(manifest: dict, locator: str) -> list[str]:
    match = re.fullmatch(r"(frozen|predecessor):([^:]+):(\d+)-(\d+)", locator)
    refuse(match is None, f"unparseable source locator: {locator}")
    authority, path_text, first_text, last_text = match.groups()
    path = safe_repo_path(path_text, authority)
    first, last = int(first_text), int(last_text)
    refuse(first < 1 or last < first, f"invalid source range: {locator}")
    if authority == "frozen":
        commit = manifest["frozen_product"]["commit"]
    else:
        commit = manifest["predecessor"]["packet_commit"]
    data = git("show", f"{commit}:{path}").decode()
    lines = data.splitlines()
    refuse(last > len(lines), f"source range exceeds file: {locator}")
    return lines[first - 1:last]


def resolve_manifest_pointer(manifest: dict, locator: str) -> object:
    refuse(not locator.startswith("manifest:/"), f"unparseable manifest locator: {locator}")
    value: object = manifest
    for raw_token in locator[len("manifest:/"):].split("/"):
        token = raw_token.replace("~1", "/").replace("~0", "~")
        if isinstance(value, dict):
            refuse(token not in value, f"missing manifest pointer target: {locator}")
            value = value[token]
        elif isinstance(value, list):
            refuse(not token.isdigit(), f"non-numeric manifest array index: {locator}")
            index = int(token)
            refuse(index >= len(value), f"manifest array index out of range: {locator}")
            value = value[index]
        else:
            raise Refused(f"manifest pointer traverses a scalar: {locator}")
    return value


def resolve_packet_path(locator: str) -> Path:
    refuse(not locator.startswith("packet:"), f"unparseable packet locator: {locator}")
    relative = Path(safe_repo_path(locator[len("packet:"):], "packet"))
    resolved = (ROOT / relative).resolve()
    refuse(resolved != ROOT and ROOT.resolve() not in resolved.parents,
           f"packet locator escapes root: {locator}")
    refuse(not resolved.is_file(), f"missing packet locator target: {locator}")
    return resolved


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
        f"Revision successor to `{manifest['revision_predecessor']['packet_commit']}` / manifest `{manifest['revision_predecessor']['manifest_sha256']}`.",
        f"Reopening reason: {manifest['reopening_reason']}",
        "",
        "## Scope", "", manifest["scope"], "",
        f"Implementation authorized: `{str(manifest['implementation_authorized']).lower()}`.",
        "", "## Blocked by", "",
    ]
    lines.extend(f"- {item}" for item in manifest["blocked_by"])
    lines.extend(["", "## Review history", ""])
    for review in manifest["review_history"]:
        lines.append(
            f"- `{review['packet_commit']}`: semantic `{review['semantic']}`, evidence `{review['evidence']}`. {review['resolution']}"
        )
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
    refuse(manifest["schema"] != "ae-p1.2-g2b-item15-packet/3", "schema changed")
    refuse(manifest["ticket"] != "AE-P1.2-G2B-ITEM15", "ticket changed")
    refuse(manifest["status"] != "REVIEW_CANDIDATE", "packet is not a review candidate")
    refuse(manifest["implementation_authorized"] is not False,
           "item 15 packet must not authorize implementation")
    refuse(len(manifest["blocked_by"]) != 2, "implementation blockers changed")
    refuse(manifest["frozen_product"] != EXPECTED_FROZEN, "frozen product identity changed")
    refuse(manifest["program_source"] != EXPECTED_PROGRAM, "program source identity changed")
    refuse(manifest["predecessor"] != EXPECTED_PREDECESSOR,
           "settled predecessor identity changed")
    refuse(manifest["revision_predecessor"] != EXPECTED_REVISION_PREDECESSOR,
           "revision predecessor identity changed")
    refuse(manifest["review_history"] != [
        {
            "packet_commit": "4a70972ac468d7c1320e95e940b3d4fbcbdd829c",
            "semantic": "BLOCKED",
            "evidence": "BLOCKED",
            "resolution": "978dd9e3 added PASS 3/item 16 supersession, bounded confounder language, stale-offline-waiter coverage, manifest/packet locator resolution, and four new mutations.",
        },
        {
            "packet_commit": "978dd9e31290551f343b581953c893cf15200c49",
            "semantic": "PASS",
            "evidence": "BLOCKED",
            "resolution": "This schema-v2 successor constrains repository paths, reads frozen evidence from the pinned commit, compares governed hashes to pinned blobs and current packet bytes, and adds two structural mutations.",
        },
        {
            "packet_commit": "1f86e0015f83c666bb2f925eaeb6105a6011b622",
            "semantic": "PASS",
            "evidence": "BLOCKED",
            "resolution": "This schema-v3 successor binds the intended external identities, exact governed population, and exact non-empty locator set for every record, with removal and valid-substitution mutations.",
        },
    ], "review history changed")

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
        refuse(record["source_span"] != EXPECTED_SOURCE_SPANS[record["id"]],
               f"source locator set changed: {record['id']}")
        spans = record["source_span"] if isinstance(record["source_span"], list) else [record["source_span"]]
        refuse(not spans or any(not isinstance(span, str) or not span for span in spans),
               f"empty source locator set: {record['id']}")
        for span in spans:
            if span.startswith("manifest:/"):
                resolve_manifest_pointer(manifest, span)
                continue
            if span.startswith("packet:"):
                resolve_packet_path(span)
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
    refuse("waiter acquires but sends no ProcessBlock before relaunch" not in failure_case["statement"],
           "stale offline waiter after partial-frame failure is not tested")

    caller = by_id["R-CALLER-HELD"]["statement"]
    refuse("unique_lock" not in caller or "never locks controllerMutex internally" not in caller,
           "caller-held lock contract weakened")
    refuse("partial SOCK_STREAM frame" not in by_id["R-FAILURE-WITHDRAWS"]["statement"],
           "partial frame failure semantics omitted")
    refuse("disconnects the transport-poisoned controller under the same controller lock" not in
           by_id["R-FAILURE-WITHDRAWS"]["statement"],
           "transport poison is not disconnected before lock release")
    confounder = by_id["E-PROBE-CONFOUNDER"]["statement"]
    refuse("This identifies confounders; it does not establish which path either historical run executed." not in confounder,
           "probe confounder was promoted into observed history")
    supersession = by_id["R-SUPERSEDE-PASS3"]["statement"]
    refuse("Predecessor PASS 3 is superseded." not in supersession or
           "hookEntryHostReady must be FALSE" not in supersession,
           "PASS 3/item 16 supersession weakened")
    refuse("does not authorize" not in by_id["G-ITEM15"]["statement"],
           "gate overstates authorization")

    governed = manifest["governed_files"]
    refuse(not isinstance(governed, list), "governed_files is not a list")
    for entry in governed:
        refuse(not isinstance(entry, dict) or set(entry) != {"path", "sha256"},
               "governed file entry shape changed")
        refuse(not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]),
               f"invalid governed digest: {entry.get('path')}")
    paths = [entry["path"] for entry in governed]
    refuse(tuple(paths) != EXPECTED_GOVERNED_PATHS,
           "governed file population changed")

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
        revision = manifest["revision_predecessor"]
        refuse(object_tree(revision["packet_commit"]) != revision["packet_tree"],
               "revision predecessor packet tree mismatch")
        revision_bytes = git("show", f"{revision['packet_commit']}:{MANIFEST_PATH.relative_to(ROOT)}")
        refuse(sha256(revision_bytes) != revision["manifest_sha256"],
               "revision predecessor manifest digest mismatch")

        for entry in governed:
            governed_path = safe_repo_path(entry["path"], "governed")
            path = ROOT / governed_path
            refuse(not path.is_file(), f"missing governed file: {entry['path']}")
            frozen_bytes = git("show", f"{frozen['commit']}:{governed_path}")
            refuse(sha256(frozen_bytes) != entry["sha256"],
                   f"governed frozen-blob mismatch: {entry['path']}")
            refuse(sha256(path.read_bytes()) != entry["sha256"],
                   f"governed packet-checkout drift: {entry['path']}")

        produce = "\n".join(source_lines(
            manifest, "frozen:apps/engine_produce_block.cpp:1072-1100"))
        refuse("if (!sentOk) {" not in produce or
               "runtime->hostReady.store(false, std::memory_order_release);" not in produce or
               "runtime->needsRestart.store(true, std::memory_order_release);" not in produce,
               "bounded ProcessBlock failure excerpt no longer withdraws readiness")
        apply = "\n".join(source_lines(
            manifest, "frozen:apps/daw_engine_main.cpp:1058-1061"))
        refuse("auto applyHostBypassStates" not in apply or
               "if (!runtime.hostReady.load(std::memory_order_acquire))" not in apply or
               "return;" not in apply,
               "bounded bypass guard excerpt changed")
        probe = "\n".join(source_lines(
            manifest, "frozen:tools/bypass_send_probe.sh:92-108"))
        positions = [probe.find(token) for token in
                     ("do play", "kill -STOP", "sleep 5", "do set-bypass")]
        refuse(any(position < 0 for position in positions) or positions != sorted(positions),
               "probe no longer orders ProcessBlock traffic and delay before bypass")

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

    bad_pointer = copy.deepcopy(manifest)
    next(r for r in bad_pointer["records"] if r["id"] == "R-NO-AUTHORIZATION")["source_span"] = \
        "manifest:/does_not_exist"
    cases.append(("missing manifest pointer", bad_pointer))

    bad_packet_path = copy.deepcopy(manifest)
    next(r for r in bad_packet_path["records"] if r["id"] == "CTRL-PACKET")["source_span"] = \
        "packet:tools/architecture/absent_item15_checker.py"
    cases.append(("absent packet path", bad_packet_path))

    overclaim = copy.deepcopy(manifest)
    next(r for r in overclaim["records"] if r["id"] == "E-PROBE-CONFOUNDER")["statement"] = \
        "Both historical variants executed the same non-send path."
    cases.append(("probe overclaim", overclaim))

    old_pass3 = copy.deepcopy(manifest)
    next(r for r in old_pass3["records"] if r["id"] == "R-SUPERSEDE-PASS3")["statement"] = \
        "hookEntryHostReady must be TRUE."
    cases.append(("PASS 3 supersession lost", old_pass3))

    traversal = copy.deepcopy(manifest)
    next(r for r in traversal["records"] if r["id"] == "E-PROBE-CONFOUNDER")["source_span"][0] = \
        "frozen:../outside:1-1"
    cases.append(("frozen path traversal", traversal))

    absolute = copy.deepcopy(manifest)
    next(r for r in absolute["records"] if r["id"] == "E-PROBE-CONFOUNDER")["source_span"][0] = \
        "frozen:/etc/passwd:1-1"
    cases.append(("absolute frozen path", absolute))

    self_updated_hash = copy.deepcopy(manifest)
    self_updated_hash["governed_files"][0]["sha256"] = "0" * 64
    cases.append(("self-updated governed hash", self_updated_hash, True))

    moved_frozen = copy.deepcopy(manifest)
    moved_frozen["frozen_product"] = copy.deepcopy(EXPECTED_PROGRAM)
    cases.append(("moved frozen identity", moved_frozen))

    removed_governed = copy.deepcopy(manifest)
    removed_governed["governed_files"].pop()
    cases.append(("governed population removal", removed_governed))

    substituted_governed = copy.deepcopy(manifest)
    substituted_governed["governed_files"][0]["path"] = "apps/device_chain.cpp"
    cases.append(("governed population substitution", substituted_governed))

    empty_locators = copy.deepcopy(manifest)
    next(r for r in empty_locators["records"] if r["id"] == "E-PROBE-CONFOUNDER")["source_span"] = []
    cases.append(("source locator deletion", empty_locators))

    substituted_locator = copy.deepcopy(manifest)
    next(r for r in substituted_locator["records"] if r["id"] == "R-NO-AUTHORIZATION")["source_span"] = \
        "manifest:/scope"
    cases.append(("valid source locator substitution", substituted_locator))

    for case in cases:
        name, candidate = case[0], case[1]
        verify_files = bool(case[2]) if len(case) == 3 else False
        try:
            validate(candidate, verify_files=verify_files, verify_prose=False)
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
    print(f"  mutation controls: 16/16 refused")
    print("  implementation authorized: false")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Refused, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"AE-P1.2 G2-B item 15 packet: REFUSED: {exc}", file=sys.stderr)
        raise SystemExit(1)
