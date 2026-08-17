#!/usr/bin/env python3
"""Structural and evidence checker for the AE-P1.2 G2-B item-18 packet."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = ROOT / "docs/architecture/tasks/AE-P1.2-g2b-item18-manifest.json"
PROSE_PATH = ROOT / "docs/architecture/tasks/AE-P1.2-g2b-item18.md"
PREDECESSOR_MANIFEST = "docs/architecture/tasks/AE-P1.2-manifest.json"
ITEM15_MANIFEST = "docs/architecture/tasks/AE-P1.2-g2b-item15-manifest.json"

TOP_LEVEL_KEYS = {
    "schema", "ticket", "status", "owner", "predecessor", "item15",
    "program_source", "frozen_product", "scope", "implementation_authorization",
    "non_goals", "governed_files", "populations", "changed_records", "records",
    "test_cases",
}
REQUIRED_RECORDS = {
    "G-ITEM18", "DEP-PREDECESSOR", "DEP-ITEM15", "DEP-FROZEN-BASE",
    "P-READINESS-PUBLISHERS", "P-SNAPSHOT-PUBLISHERS", "P-HOST-PLAN-MUTATIONS",
    "R-BYPASS-STAGED", "R-HOST-PLAN-AUTHORITY", "R-DISPATCH-TICKET",
    "R-MIRROR-EPOCH", "R-PASS4-REPLACEMENT", "R-OFFLINE-PRIMER", "R-G4-WITNESS",
    "D-PRODUCTION-FIXTURE", "R-REVIEW-GATED-AUTH", "CTRL-PACKET", "CTRL-MUTATIONS",
}
REQUIRED_TESTS = {
    "T-COLD-REFUSES", "T-ALL-PUBLISHERS", "T-PLAN-RACE", "T-EXACT-SLOTS",
    "T-BYPASS-FAILURE", "T-LOWER-PRIMER", "T-PRIMED-NOT-COMPLETE",
    "T-ACK-BOUNDARY", "T-REARM-REGRESSES", "T-TRACK-LOCAL",
    "T-OFFLINE-NO-LEAK", "T-G4-WITNESS",
}
EXPECTED_PREDECESSOR = {
    "packet_commit": "2b5f0747f1b7dde79ae788af3826c49c78df5d2a",
    "packet_tree": "7c75beb7b941c06a6099292fbf4dac6ade503a6a",
    "manifest_sha256": "c321130b860fda73991f04d1035bea7af03faf6e030fce5565664c1657ce093e",
}
EXPECTED_ITEM15 = {
    "packet_commit": "8ee5b3cdd34ef6c5538fac19074b4f442c0a8514",
    "packet_tree": "44ce562e2463ba0b6fcc291077b38fc7d87c07a1",
    "manifest_sha256": "a9583a4cb8a8fd45d1dc2ccfb683cf06f8758c55407bfdf6f3d5a424eea4465f",
}
EXPECTED_PROGRAM = {
    "commit": "4f343790562c5c9e6b43e07cacf647789856d2f4",
    "tree": "4ff89dbca12d20a6ce46f8099a0db8b059b3af21",
}
EXPECTED_FROZEN = {
    "commit": "92dfdfe23cc7ff93f2ce14894a35d089e3d9e2b8",
    "tree": "238ac970b5d61fe16055ede4c43a2978ddb11da7",
}
EXPECTED_GOVERNED_PATHS = (
    "apps/daw_engine_main.cpp",
    "apps/device_chain.cpp",
    "apps/engine_arrangetime_commands.cpp",
    "apps/engine_audio_callback.h",
    "apps/engine_automation_commands.cpp",
    "apps/engine_bulk_edit.cpp",
    "apps/engine_chain_commands.cpp",
    "apps/engine_chain_host.cpp",
    "apps/engine_consumer.cpp",
    "apps/engine_load_patcher_pool.cpp",
    "apps/engine_load_project.cpp",
    "apps/engine_load_track.cpp",
    "apps/engine_master_render.cpp",
    "apps/engine_mirror_replay.h",
    "apps/engine_modlink_commands.cpp",
    "apps/engine_patcher_assemble.cpp",
    "apps/engine_patcher_commands.cpp",
    "apps/engine_produce_block.cpp",
    "apps/engine_producer_thread.cpp",
    "apps/engine_readiness_level.h",
    "apps/engine_readiness_tests_main.cpp",
    "apps/engine_restart_worker.cpp",
    "apps/engine_rt_helpers.cpp",
    "apps/engine_sampler_commands.cpp",
    "apps/engine_song_store.cpp",
    "apps/engine_track_commands.cpp",
    "apps/engine_track_setup.cpp",
    "apps/engine_trackprops_commands.cpp",
    "apps/engine_types.h",
    "apps/engine_ui_publish.cpp",
    "apps/host_controller.cpp",
    "apps/ipc_io.cpp",
)
EXPECTED_READY_SITES = (
    ("apps/engine_track_setup.cpp", 64, "setupTrackRuntime",
     "prepublication object; publishes before the chain is constructed"),
    ("apps/engine_track_setup.cpp", 429, "restartTrackHost",
     "published runtime under controllerMutex; no bypass staging"),
    ("apps/engine_restart_worker.cpp", 143, "runRestartWorker",
     "published runtime; store occurs after controller unlock and before bypass staging"),
)
EXPECTED_SNAPSHOT_SITES = (
    ("apps/daw_engine_main.cpp", 414, "plain_prepublication"),
    ("apps/daw_engine_main.cpp", 1100, "plain_prepublication"),
    ("apps/engine_arrangetime_commands.cpp", 380, "atomic"),
    ("apps/engine_automation_commands.cpp", 56, "atomic"),
    ("apps/engine_automation_commands.cpp", 237, "atomic"),
    ("apps/engine_automation_commands.cpp", 333, "atomic"),
    ("apps/engine_chain_commands.cpp", 168, "atomic"),
    ("apps/engine_consumer.cpp", 592, "atomic"),
    ("apps/engine_load_project.cpp", 392, "atomic"),
    ("apps/engine_load_project.cpp", 504, "atomic"),
    ("apps/engine_load_track.cpp", 139, "atomic"),
    ("apps/engine_modlink_commands.cpp", 100, "atomic"),
    ("apps/engine_modlink_commands.cpp", 231, "atomic"),
    ("apps/engine_modlink_commands.cpp", 283, "atomic"),
    ("apps/engine_patcher_commands.cpp", 192, "atomic"),
    ("apps/engine_patcher_commands.cpp", 421, "atomic"),
    ("apps/engine_song_store.cpp", 117, "atomic"),
    ("apps/engine_track_commands.cpp", 95, "atomic"),
    ("apps/engine_track_commands.cpp", 184, "atomic"),
    ("apps/engine_track_commands.cpp", 238, "atomic"),
    ("apps/engine_track_setup.cpp", 100, "plain_prepublication"),
    ("apps/engine_track_setup.cpp", 281, "atomic"),
    ("apps/engine_trackprops_commands.cpp", 70, "atomic_unlocked_build"),
    ("apps/engine_trackprops_commands.cpp", 99, "atomic_unlocked_build"),
)
EXPECTED_HOST_PLAN_ROOTS = (
    ("apps/engine_track_setup.cpp", 69, "setupTrackRuntime", "host_plan"),
    ("apps/engine_chain_commands.cpp", 100, "handleAddDevice/addDevice", "execution_plan_topology"),
    ("apps/engine_chain_commands.cpp", 108, "handleAddDevice/removeDeviceById", "execution_plan_topology"),
    ("apps/engine_chain_commands.cpp", 115, "handleAddDevice/moveDeviceById", "execution_plan_topology"),
    ("apps/engine_chain_commands.cpp", 125, "handleAddDevice/setDeviceBypass", "execution_plan_vst_or_patcher_audio"),
    ("apps/engine_chain_commands.cpp", 135, "handleAddDevice/setDeviceHostSlotIndex", "host_plan_when_vst"),
    ("apps/daw_engine_main.cpp", 1209, "updateTrackChainForInstrument", "host_plan_missing_snapshot_publication"),
    ("apps/engine_load_track.cpp", 86, "loadTrackFromDocument", "host_plan"),
    ("apps/engine_load_project.cpp", 498, "applyDocument/master", "host_plan"),
    ("apps/engine_rt_helpers.cpp", 76, "resetTrackContent", "host_plan_missing_snapshot_on_leftover_clear"),
    ("apps/engine_track_commands.cpp", 344, "handleRemoveTrack", "host_plan_missing_snapshot_publication"),
    ("apps/engine_patcher_assemble.cpp", 90, "reassemblePatcherFromDevices", "execution_plan_patcher_node_missing_publication"),
    ("apps/engine_load_patcher_pool.cpp", 103, "loadPatcherPool/runtime repoint", "execution_plan_patcher_node_missing_publication"),
    ("apps/engine_consumer.cpp", 573, "reconcileChildTracks/restore", "hostless_aux_explicit_exclusion"),
    ("apps/daw_engine_main.cpp", 1099, "makeAuxChild", "hostless_aux_prepublication_explicit_exclusion"),
)
EXPECTED_NON_HOST = (
    ("apps/engine_sampler_commands.cpp", "sampler document internals; samplerSnapshot authority"),
    ("apps/engine_bulk_edit.cpp", "sampler document internals"),
    ("apps/engine_patcher_commands.cpp", "patcher graph/config internals; resulting patcherNodeId repoint is included separately"),
)
EXPECTED_SOURCE_SPANS = {
    "G-ITEM18": [
        "predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:965-984",
        "predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:1876-1890",
        "predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:2559-2569",
    ],
    "DEP-PREDECESSOR": "predecessor:docs/architecture/tasks/AE-P1.2-manifest.json:1-40",
    "DEP-ITEM15": "item15:docs/architecture/tasks/AE-P1.2-g2b-item15-manifest.json:1-40",
    "DEP-FROZEN-BASE": "frozen:apps/engine_readiness_level.h:1-105",
    "P-READINESS-PUBLISHERS": "manifest:/populations/host_ready_true_sites",
    "P-SNAPSHOT-PUBLISHERS": "manifest:/populations/track_snapshot_publications",
    "P-HOST-PLAN-MUTATIONS": [
        "manifest:/populations/host_plan_mutation_roots",
        "manifest:/populations/classified_non_host_chain_mutations",
    ],
    "R-BYPASS-STAGED": [
        "frozen:apps/host_controller.cpp:624-654",
        "frozen:apps/ipc_io.cpp:89-155",
    ],
    "R-HOST-PLAN-AUTHORITY": [
        "frozen:apps/daw_engine_main.cpp:319-329",
        "frozen:apps/engine_produce_block.cpp:361-398",
        "frozen:apps/engine_chain_host.cpp:142-272",
    ],
    "R-DISPATCH-TICKET": [
        "frozen:apps/engine_audio_callback.h:34-56",
        "frozen:apps/engine_consumer.cpp:636-665",
        "frozen:apps/engine_restart_worker.cpp:102-165",
    ],
    "R-MIRROR-EPOCH": [
        "frozen:apps/engine_rt_helpers.cpp:33-56",
        "frozen:apps/engine_produce_block.cpp:334-343",
        "frozen:apps/engine_produce_block.cpp:508-516",
        "frozen:apps/engine_producer_thread.cpp:199-229",
    ],
    "R-PASS4-REPLACEMENT": "manifest:/test_cases",
    "R-OFFLINE-PRIMER": [
        "frozen:apps/engine_audio_callback.h:900-980",
        "frozen:apps/engine_master_render.cpp:1-150",
    ],
    "R-G4-WITNESS": "predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:1167-1174",
    "D-PRODUCTION-FIXTURE": "manifest:/test_cases",
    "R-REVIEW-GATED-AUTH": "manifest:/implementation_authorization",
    "CTRL-PACKET": "packet:tools/architecture/ae_p1_2_g2b_item18_check.py",
    "CTRL-MUTATIONS": "packet:tools/architecture/ae_p1_2_g2b_item18_check.py",
}


class Refused(RuntimeError):
    pass


def refuse(condition: bool, message: str) -> None:
    if condition:
        raise Refused(message)


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=ROOT)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def object_tree(commit: str) -> str:
    return git("show", "-s", "--format=%T", commit).decode().strip()


def safe_repo_path(path_text: str, authority: str) -> str:
    refuse(not path_text or "\\" in path_text, f"unsafe {authority} path: {path_text}")
    relative = Path(path_text)
    refuse(relative.is_absolute() or ".." in relative.parts or "." in relative.parts,
           f"unsafe {authority} path: {path_text}")
    refuse(relative.as_posix() != path_text, f"non-canonical {authority} path: {path_text}")
    return path_text


def source_lines(manifest: dict, locator: str) -> list[str]:
    match = re.fullmatch(r"(frozen|predecessor|item15):([^:]+):(\d+)-(\d+)", locator)
    refuse(match is None, f"unparseable source locator: {locator}")
    authority, path_text, first_text, last_text = match.groups()
    path = safe_repo_path(path_text, authority)
    first, last = int(first_text), int(last_text)
    refuse(first < 1 or last < first, f"invalid source range: {locator}")
    commits = {
        "frozen": manifest["frozen_product"]["commit"],
        "predecessor": manifest["predecessor"]["packet_commit"],
        "item15": manifest["item15"]["packet_commit"],
    }
    lines = git("show", f"{commits[authority]}:{path}").decode().splitlines()
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
            refuse(not token.isdigit(), f"non-numeric manifest index: {locator}")
            index = int(token)
            refuse(index >= len(value), f"manifest index out of range: {locator}")
            value = value[index]
        else:
            raise Refused(f"manifest pointer traverses scalar: {locator}")
    return value


def resolve_packet_path(locator: str) -> Path:
    refuse(not locator.startswith("packet:"), f"unparseable packet locator: {locator}")
    relative = Path(safe_repo_path(locator[len("packet:"):], "packet"))
    resolved = (ROOT / relative).resolve()
    refuse(ROOT.resolve() not in resolved.parents, f"packet locator escapes root: {locator}")
    refuse(not resolved.is_file(), f"missing packet locator target: {locator}")
    return resolved


def render(manifest: dict) -> str:
    auth = manifest["implementation_authorization"]
    populations = manifest["populations"]
    lines = [
        "# AE-P1.2 G2-B — item 18 readiness successor",
        "",
        "> Generated from `AE-P1.2-g2b-item18-manifest.json`; do not edit by hand.",
        "",
        f"Status: `{manifest['status']}`. Owner: `{manifest['owner']}`.",
        f"Frozen product: `{manifest['frozen_product']['commit']}` (tree `{manifest['frozen_product']['tree']}`).",
        f"Item-15 input: `{manifest['item15']['packet_commit']}` / manifest `{manifest['item15']['manifest_sha256']}`.",
        "",
        "## Scope", "", manifest["scope"], "",
        "Implementation authorized before dual PASS: "
        f"`{str(auth['before_dual_pass']).lower()}`. The same-SHA dual PASS authorizes the declared scope: "
        f"`{str(auth['after_same_sha_semantic_and_evidence_pass']).lower()}`.",
        "",
        "## Frozen populations", "",
        f"- Readiness-true publishers: `{len(populations['host_ready_true_sites'])}`.",
        f"- TrackStateSnapshot publications: `{len(populations['track_snapshot_publications'])}`.",
        f"- Hosted-plan mutation roots: `{len(populations['host_plan_mutation_roots'])}`.",
        f"- Explicit non-host mutation families: `{len(populations['classified_non_host_chain_mutations'])}`.",
        "",
        "## Records", "",
    ]
    for record in manifest["records"]:
        lines.append(f"- `{record['id']}` [{record['kind']} / {record['status']}]: {record['statement']}")
    lines.extend(["", "## Required implementation tests", ""])
    for case in manifest["test_cases"]:
        lines.append(f"- `{case['id']}`: {case['statement']}")
    lines.extend(["", "## Non-goals", ""])
    lines.extend(f"- {item}" for item in manifest["non_goals"])
    lines.extend([
        "", "## Review requirement", "",
        "Independent semantic and evidence reviewers must both return PASS for the same immutable packet SHA and frozen product before the declared implementation authorization becomes effective.",
        "",
    ])
    return "\n".join(lines)


def tuple_ready(entries: list[dict]) -> tuple[tuple, ...]:
    return tuple((e.get("path"), e.get("line"), e.get("symbol"), e.get("classification"))
                 for e in entries)


def tuple_snapshot(entries: list[dict]) -> tuple[tuple, ...]:
    return tuple((e.get("path"), e.get("line"), e.get("kind")) for e in entries)


def tuple_non_host(entries: list[dict]) -> tuple[tuple, ...]:
    return tuple((e.get("path"), e.get("classification")) for e in entries)


def validate(manifest: dict, *, verify_files: bool = True, verify_prose: bool = True) -> None:
    refuse(set(manifest) != TOP_LEVEL_KEYS, "top-level manifest shape changed")
    refuse(manifest["schema"] != "ae-p1.2-g2b-item18-packet/1", "schema changed")
    refuse(manifest["ticket"] != "AE-P1.2-G2B-ITEM18", "ticket changed")
    refuse(manifest["status"] != "REVIEW_CANDIDATE", "status changed")
    refuse(manifest["owner"] != "backend", "owner changed")
    refuse(manifest["predecessor"] != EXPECTED_PREDECESSOR, "predecessor identity changed")
    refuse(manifest["item15"] != EXPECTED_ITEM15, "item-15 identity changed")
    refuse(manifest["program_source"] != EXPECTED_PROGRAM, "program identity changed")
    refuse(manifest["frozen_product"] != EXPECTED_FROZEN, "frozen product changed")

    authorization = manifest["implementation_authorization"]
    refuse(set(authorization) != {
        "before_dual_pass", "after_same_sha_semantic_and_evidence_pass", "scope"
    }, "authorization shape changed")
    refuse(authorization["before_dual_pass"] is not False,
           "implementation authorized before dual PASS")
    refuse(authorization["after_same_sha_semantic_and_evidence_pass"] is not True,
           "same-SHA dual PASS no longer authorizes declared implementation")
    refuse("item-15 and item-18" not in authorization["scope"],
           "implementation scope changed")

    populations = manifest["populations"]
    refuse(set(populations) != {
        "host_ready_true_sites", "track_snapshot_publications",
        "host_plan_mutation_roots", "classified_non_host_chain_mutations"
    }, "population classes changed")
    refuse(tuple_ready(populations["host_ready_true_sites"]) != EXPECTED_READY_SITES,
           "readiness publisher population changed")
    refuse(tuple_snapshot(populations["track_snapshot_publications"]) != EXPECTED_SNAPSHOT_SITES,
           "snapshot publisher population changed")
    refuse(tuple_ready(populations["host_plan_mutation_roots"]) != EXPECTED_HOST_PLAN_ROOTS,
           "host-plan mutation population changed")
    refuse(tuple_non_host(populations["classified_non_host_chain_mutations"]) != EXPECTED_NON_HOST,
           "classified non-host population changed")

    governed = manifest["governed_files"]
    refuse(not isinstance(governed, list), "governed_files is not a list")
    paths = tuple(entry.get("path") for entry in governed)
    refuse(paths != EXPECTED_GOVERNED_PATHS, "governed-file population changed")
    for entry in governed:
        refuse(set(entry) != {"path", "sha256"}, f"governed entry shape: {entry.get('path')}")
        refuse(re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) is None,
               f"invalid governed digest: {entry['path']}")

    records = manifest["records"]
    ids = [record.get("id") for record in records]
    refuse(len(ids) != len(set(ids)), "duplicate record id")
    refuse(set(ids) != REQUIRED_RECORDS, "record set changed")
    refuse(manifest["changed_records"] != sorted(REQUIRED_RECORDS),
           "changed_records is not the exact sorted record set")
    by_id = {record["id"]: record for record in records}
    for record in records:
        refuse(set(record) != {
            "id", "kind", "owner", "status", "dependencies", "source_span", "control", "statement"
        }, f"record shape changed: {record.get('id')}")
        refuse(record["owner"] != "backend", f"record owner changed: {record['id']}")
        refuse(record["control"] not in REQUIRED_RECORDS, f"unknown control: {record['id']}")
        refuse(record["source_span"] != EXPECTED_SOURCE_SPANS[record["id"]],
               f"source locator set changed: {record['id']}")
        for dependency in record["dependencies"]:
            refuse(dependency not in by_id, f"unknown dependency {dependency}: {record['id']}")
        spans = record["source_span"] if isinstance(record["source_span"], list) else [record["source_span"]]
        refuse(not spans, f"empty source locator set: {record['id']}")
        for locator in spans:
            if locator.startswith("manifest:/"):
                resolve_manifest_pointer(manifest, locator)
            elif locator.startswith("packet:"):
                resolve_packet_path(locator)
            else:
                source_lines(manifest, locator)

    visiting: set[str] = set()
    visited: set[str] = set()
    def walk(record_id: str) -> None:
        refuse(record_id in visiting, f"dependency cycle at {record_id}")
        if record_id in visited:
            return
        visiting.add(record_id)
        for dependency in by_id[record_id]["dependencies"]:
            walk(dependency)
        visiting.remove(record_id)
        visited.add(record_id)
    walk("R-REVIEW-GATED-AUTH")
    refuse(visited != REQUIRED_RECORDS, "authorization dependency closure is incomplete")

    test_ids = [case.get("id") for case in manifest["test_cases"]]
    refuse(len(test_ids) != len(set(test_ids)), "duplicate test id")
    refuse(set(test_ids) != REQUIRED_TESTS, "test set changed")
    for case in manifest["test_cases"]:
        refuse(set(case) != {"id", "statement"}, f"test shape changed: {case.get('id')}")

    bypass = by_id["R-BYPASS-STAGED"]["statement"]
    refuse("not a claim that the plugin acknowledged" not in bypass or
           "transmitted completely" not in bypass or
           "partial-frame-poisoned" not in bypass,
           "bypass staging semantics weakened")
    host_plan = by_id["R-HOST-PLAN-AUTHORITY"]["statement"]
    refuse("dedicated immutable AuthoredHostExecutionPlan" not in host_plan or
           "exact full ordered topology" not in host_plan or
           "reloads the current plan" not in host_plan or
           "no longer an authority for hosted segmentation" not in host_plan,
           "host-plan authority or post-lock validation weakened")
    ticket = by_id["R-DISPATCH-TICKET"]["statement"]
    refuse("nonzero uint64 dispatch ticket" not in ticket or
           "rechecks it after acquiring" not in ticket or
           "stale offline waiter" not in ticket,
           "dispatch-ticket contract weakened")
    mirror = by_id["R-MIRROR-EPOCH"]["statement"]
    refuse("re-entrant generation" not in mirror or
           "ack >= the exact nonzero gate" not in mirror or
           "same dispatch-ticket/epoch" not in mirror or
           "stale ack cannot promote" not in mirror,
           "mirror epoch or stale-ack contract weakened")
    pass4 = by_id["R-PASS4-REPLACEMENT"]["statement"]
    refuse("control-only primer ProcessBlock" not in pass4 or
           "remain refused until the exact gate ack" not in pass4 or
           "Unrelated tracks continue" not in pass4,
           "PASS 4 replacement weakened")
    offline = by_id["R-OFFLINE-PRIMER"]["statement"]
    refuse("before timeline block zero" not in offline or
           "do not advance musical transport" not in offline or
           "never stop unrelated tracks" not in offline,
           "offline or track-local primer contract weakened")
    refuse(not any("No global mirrorOnly gate" in item for item in manifest["non_goals"]),
           "global mirror gating is no longer forbidden")

    ack_test = next(case for case in manifest["test_cases"] if case["id"] == "T-ACK-BOUNDARY")
    refuse("gate-minus-one" not in ack_test["statement"] or
           "exact nonzero gate" not in ack_test["statement"],
           "ack boundary is no longer exact")
    rearm_test = next(case for case in manifest["test_cases"] if case["id"] == "T-REARM-REGRESSES")
    refuse("prior epoch's ack" not in rearm_test["statement"], "stale re-arm control omitted")
    offline_test = next(case for case in manifest["test_cases"] if case["id"] == "T-OFFLINE-NO-LEAK")
    refuse("captured output" not in offline_test["statement"], "offline output leak is untested")

    if verify_files:
        refuse(object_tree(EXPECTED_FROZEN["commit"]) != EXPECTED_FROZEN["tree"],
               "frozen product tree mismatch")
        refuse(object_tree(EXPECTED_PREDECESSOR["packet_commit"]) != EXPECTED_PREDECESSOR["packet_tree"],
               "predecessor tree mismatch")
        refuse(object_tree(EXPECTED_ITEM15["packet_commit"]) != EXPECTED_ITEM15["packet_tree"],
               "item-15 tree mismatch")
        refuse(object_tree(EXPECTED_PROGRAM["commit"]) != EXPECTED_PROGRAM["tree"],
               "program tree mismatch")
        predecessor_bytes = git("show", f"{EXPECTED_PREDECESSOR['packet_commit']}:{PREDECESSOR_MANIFEST}")
        refuse(sha256(predecessor_bytes) != EXPECTED_PREDECESSOR["manifest_sha256"],
               "predecessor manifest digest mismatch")
        item15_bytes = git("show", f"{EXPECTED_ITEM15['packet_commit']}:{ITEM15_MANIFEST}")
        refuse(sha256(item15_bytes) != EXPECTED_ITEM15["manifest_sha256"],
               "item-15 manifest digest mismatch")
        for entry in governed:
            path = safe_repo_path(entry["path"], "governed")
            frozen_bytes = git("show", f"{EXPECTED_FROZEN['commit']}:{path}")
            refuse(sha256(frozen_bytes) != entry["sha256"], f"frozen blob mismatch: {path}")
            checkout = ROOT / path
            refuse(not checkout.is_file(), f"missing governed checkout file: {path}")
            refuse(sha256(checkout.read_bytes()) != entry["sha256"], f"checkout drift: {path}")

        raw_ready = git("grep", "-n", "-F", "hostReady.store(true", EXPECTED_FROZEN["commit"], "--", "apps").decode().splitlines()
        production_ready = [line for line in raw_ready if "_tests_main" not in line]
        observed_ready = []
        for line in production_ready:
            match = re.fullmatch(r"[^:]+:([^:]+):(\d+):.*", line)
            refuse(match is None, f"unparseable readiness grep line: {line}")
            observed_ready.append((match.group(1), int(match.group(2))))
        refuse(tuple(sorted(observed_ready)) !=
               tuple(sorted((p, n) for p, n, _, _ in EXPECTED_READY_SITES)),
               "frozen readiness publisher census drifted")

        observed_snapshots = []
        for path in EXPECTED_GOVERNED_PATHS:
            if not path.endswith((".cpp", ".h")):
                continue
            text = git("show", f"{EXPECTED_FROZEN['commit']}:{path}").decode()
            for match in re.finditer(r"(?:std::)?atomic_store_explicit\s*\(\s*&[A-Za-z_][A-Za-z0-9_]*->trackSnapshot", text):
                observed_snapshots.append((path, text.count("\n", 0, match.start()) + 1, "atomic"))
            for match in re.finditer(r"[A-Za-z_][A-Za-z0-9_]*->trackSnapshot\s*=\s*buildTrackSnapshot", text):
                observed_snapshots.append((path, text.count("\n", 0, match.start()) + 1, "plain_prepublication"))
        normalized_expected = tuple((p, n, "atomic" if k == "atomic_unlocked_build" else k)
                                    for p, n, k in EXPECTED_SNAPSHOT_SITES)
        refuse(tuple(sorted(observed_snapshots)) != tuple(sorted(normalized_expected)),
               "frozen snapshot publication census drifted")

        for path, line, _, _ in EXPECTED_HOST_PLAN_ROOTS:
            frozen_line = git("show", f"{EXPECTED_FROZEN['commit']}:{path}").decode().splitlines()[line - 1]
            refuse("chain" not in frozen_line and "Device" not in frozen_line and
                   "resetTrackContent" not in frozen_line and "patcherNodeId" not in frozen_line,
                   f"host-plan root anchor changed: {path}:{line}")

    if verify_prose:
        refuse(not PROSE_PATH.is_file(), "generated prose is missing")
        refuse(PROSE_PATH.read_text(encoding="utf-8") != render(manifest),
               "generated prose differs from manifest")


def self_test(manifest: dict) -> None:
    cases: list[tuple[str, dict, bool]] = []
    def add(name: str, candidate: dict, verify_files: bool = False) -> None:
        cases.append((name, candidate, verify_files))

    candidate = copy.deepcopy(manifest)
    candidate["records"] = [r for r in candidate["records"] if r["id"] != "R-MIRROR-EPOCH"]
    add("record deletion", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["implementation_authorization"]["before_dual_pass"] = True
    add("premature implementation authorization", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["implementation_authorization"]["after_same_sha_semantic_and_evidence_pass"] = False
    add("dual-PASS authorization removed", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["populations"]["host_ready_true_sites"].pop()
    add("readiness population deletion", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["populations"]["host_ready_true_sites"][0]["line"] = 66
    add("readiness population substitution", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["populations"]["track_snapshot_publications"].pop()
    add("snapshot population deletion", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["populations"]["track_snapshot_publications"][0]["line"] = 415
    add("snapshot population substitution", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["populations"]["host_plan_mutation_roots"].pop()
    add("host-plan population deletion", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["populations"]["classified_non_host_chain_mutations"].pop()
    add("non-host classification deletion", candidate)

    candidate = copy.deepcopy(manifest)
    next(r for r in candidate["records"] if r["id"] == "R-BYPASS-STAGED")["statement"] = \
        "MappedAndBypassed proves the plugin applied every state."
    add("bypass application overclaim", candidate)

    candidate = copy.deepcopy(manifest)
    next(r for r in candidate["records"] if r["id"] == "R-HOST-PLAN-AUTHORITY")["statement"] = \
        "The producer uses the first plan it loaded before taking the lock."
    add("stale-plan recheck omitted", candidate)

    candidate = copy.deepcopy(manifest)
    next(r for r in candidate["records"] if r["id"] == "R-MIRROR-EPOCH")["statement"] = \
        "mirrorPrimed means MirrorComplete."
    add("primed treated as complete", candidate)

    candidate = copy.deepcopy(manifest)
    next(c for c in candidate["test_cases"] if c["id"] == "T-ACK-BOUNDARY")["statement"] = \
        "Any acknowledgement completes the mirror."
    add("ack boundary weakened", candidate)

    candidate = copy.deepcopy(manifest)
    next(c for c in candidate["test_cases"] if c["id"] == "T-REARM-REGRESSES")["statement"] = \
        "Overflow is logged."
    add("re-arm regression control removed", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["non_goals"] = [item for item in candidate["non_goals"] if "global mirrorOnly" not in item]
    add("global mirror gating allowed", candidate)

    candidate = copy.deepcopy(manifest)
    next(r for r in candidate["records"] if r["id"] == "R-OFFLINE-PRIMER")["statement"] = \
        "Offline primers count as ordinary rendered blocks."
    add("offline primer leakage", candidate)

    candidate = copy.deepcopy(manifest)
    next(r for r in candidate["records"] if r["id"] == "R-G4-WITNESS")["source_span"] = \
        "manifest:/scope"
    add("valid locator substitution", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["governed_files"][0]["sha256"] = "0" * 64
    add("self-updated governed digest", candidate, True)

    candidate = copy.deepcopy(manifest)
    next(r for r in candidate["records"] if r["id"] == "G-ITEM18")["dependencies"].append("NO-SUCH")
    add("broken dependency", candidate)

    for name, candidate, verify_files in cases:
        try:
            validate(candidate, verify_files=verify_files, verify_prose=False)
        except Refused:
            continue
        raise Refused(f"mutation was accepted: {name}")


def main() -> int:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    if "--render" in sys.argv:
        sys.stdout.write(render(manifest))
        return 0
    validate(manifest)
    self_test(manifest)
    print("AE-P1.2 G2-B item 18 packet: PASS")
    print(f"  records: {len(manifest['records'])}")
    print(f"  governed files: {len(manifest['governed_files'])}")
    print("  readiness publishers: 3")
    print("  snapshot publications: 24")
    print("  mutation controls: 19/19 refused")
    print("  implementation before dual PASS: false")
    print("  implementation after same-SHA dual PASS: true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Refused, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"AE-P1.2 G2-B item 18 packet: REFUSED: {exc}", file=sys.stderr)
        raise SystemExit(1)
