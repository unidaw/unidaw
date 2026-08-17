#!/usr/bin/env python3
"""Structural and exact-evidence checker for AE-P1.2 G2-B item-18 schema v4."""

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
    "schema", "ticket", "status", "owner", "revision_predecessor", "review_history",
    "predecessor", "item15", "program_source", "frozen_product", "scope",
    "implementation_authorization", "non_goals", "governed_files", "populations",
    "changed_records", "records", "test_cases",
}

EXPECTED_EXTERNAL = {
    "revision_predecessor": {
        "packet_commit": "3042fdb5bffde1a0b83f192b02a63b7707d8748d",
        "packet_tree": "587af01fc781ff67330096de9615d725ff4e295b",
        "manifest_sha256": "aba7014eb49cbc69d6d41c5286c169fa49aa125672da0f96909970445c067bb5",
    },
    "predecessor": {
        "packet_commit": "2b5f0747f1b7dde79ae788af3826c49c78df5d2a",
        "packet_tree": "7c75beb7b941c06a6099292fbf4dac6ade503a6a",
        "manifest_sha256": "c321130b860fda73991f04d1035bea7af03faf6e030fce5565664c1657ce093e",
    },
    "item15": {
        "packet_commit": "8ee5b3cdd34ef6c5538fac19074b4f442c0a8514",
        "packet_tree": "44ce562e2463ba0b6fcc291077b38fc7d87c07a1",
        "manifest_sha256": "a9583a4cb8a8fd45d1dc2ccfb683cf06f8758c55407bfdf6f3d5a424eea4465f",
    },
    "program_source": {
        "commit": "4f343790562c5c9e6b43e07cacf647789856d2f4",
        "tree": "4ff89dbca12d20a6ce46f8099a0db8b059b3af21",
    },
    "frozen_product": {
        "commit": "92dfdfe23cc7ff93f2ce14894a35d089e3d9e2b8",
        "tree": "238ac970b5d61fe16055ede4c43a2978ddb11da7",
    },
}

# These are SHA-256 hashes of canonical JSON for each top-level value. They make the
# manifest immutable without copying 2,000 lines of JSON into the checker. Self-tests mutate
# the parsed manifest while these constants remain fixed, so a declaration cannot bless itself.
EXPECTED_SECTION_DIGESTS = {
    "schema": "1e84c8fa0601f90155903ad90d376183b00d4629f2470844234d415a535c8916",
    "ticket": "51fc055b4f8c131bfb5f08b4d03e9141b5667cc3f398ef763016462cf9fd1380",
    "status": "a9214bd949881802e074114cc9ffc50cbf88f003ee4a269801e41dd13fdecf5c",
    "owner": "74a80ba7942f31913df8aed6e77553465be5b3c80bc5666036f42a54a94c828a",
    "revision_predecessor": "c663fc692749421a647e6a76c93ea4bd6091391bf70b1ba24b2430b1f5af6ea7",
    "review_history": "1d1e42babd9c6d42c584d86224abf675afcfc891e0542387eeba9c5015188913",
    "predecessor": "a87219ba250bdbb90663db22afac5052bc7379e485789cc40e10abc21e8f5959",
    "item15": "0bb1bb2ed7ab992ab7607c8fb4517a26b6bdc7698e8836b8ba3e57bc33884c32",
    "program_source": "2cbd44acb7ae764d5b84e8bcb76cc3f34dd435e4d734104809d199fbb76fa212",
    "frozen_product": "4dcff51b0d360b6c2ebafb0ab2b44f94ada495a53144f15cf6ad5e220ae856de",
    "scope": "4f63e17008f8ac9972d2940b82353cfbb885d3f0700591c6bc43027f41cfc234",
    "implementation_authorization": "cc6acf10b06637ab80da6f07ee7875c334d96c200c9d4c1fec3930a3c38adb9a",
    "non_goals": "16ba0243a43b2e39e4dce871421855eb6cb63d9824c0b0b4fc93d665b5c9c3c4",
    "governed_files": "7c0887271f8b73d8ed4ea6f38dc20a85457769694c8147ba738d48aab31911a0",
    "populations": "938b793bf5dfdb5c8d8089f1cfd4abbdcd815c9932c7af73a30831aeea64430d",
    "changed_records": "46b044502edf2e58222709629909c1ce3cf5fa4c6b1c68e9031134153cc8bb0a",
    "records": "dff4537d9dc5a883da42fe93af31ac4171435c1098e6433cc56fecafcb030995",
    "test_cases": "d0e105f7845789ac8fe1e57da2a432f3289994e8dde65a3485e09224073534f7",
}

EXPECTED_COUNTS = {
    "governed_files": 76,
    "records": 32,
    "test_cases": 26,
    "host_ready_true_sites": 3,
    "track_snapshot_publications": 24,
    "host_plan_mutation_roots": 19,
    "classified_non_host_chain_mutations": 4,
    "chain_mutation_scan": 47,
    "execution_authority_lexical_scan": 57,
    "routing_authority_lexical_scan": 51,
    "mirror_identity_lexical_scan": 26,
    "device_identity_lexical_scan": 25,
    "target_identity_lexical_scan": 41,
    "host_config_lexical_scan": 28,
    "execution_authority_consumers": 68,
    "authored_document_exemptions": 13,
    "process_block_senders": 3,
    "process_block_receiver": 2,
    "replay_protocol_sites": 8,
    "replay_receiver_order_sites": 7,
    "offline_coordinator_sites": 12,
    "mapping_and_output_gates": 10,
    "capacity_contract_sites": 5,
}

EXPECTED_RECORD_IDS = {
    "G-ITEM18", "DEP-PREDECESSOR", "DEP-ITEM15", "DEP-FROZEN-BASE",
    "P-READINESS-PUBLISHERS", "P-SNAPSHOT-PUBLISHERS", "P-HOST-PLAN-MUTATIONS",
    "R-BYPASS-STAGED", "R-HOST-PLAN-AUTHORITY", "R-ROUTING-AUTHORITY",
    "R-DEVICE-ID-LIFETIME", "R-STABLE-DEVICE-TARGETS", "R-MIRROR-INSTANCE-IDENTITY",
    "R-DISPATCH-TICKET", "R-MIRROR-EPOCH", "R-R13-RECONCILIATION",
    "R-PASS4-REPLACEMENT", "R-OFFLINE-PRIMER", "R-G4-WITNESS",
    "D-PRODUCTION-FIXTURE", "R-REVIEW-GATED-AUTH", "CTRL-PACKET", "CTRL-MUTATIONS",
    "P-EXECUTION-AUTHORITY-CONSUMERS", "P-DISPATCH-PROTOCOL-SURFACES",
    "P-OFFLINE-OUTPUT-SURFACES", "R-CORRELATED-REPLAY-ACK",
    "R-ATOMIC-PRIMER-CAPACITY", "R-MASTER-CORRELATION", "R-PROTOCOL-VERSION",
    "D-PRODUCTION-RECEIVER", "R-PROJECT-TARGET-MIGRATION",
}

EXPECTED_TEST_IDS = {
    "T-COLD-REFUSES", "T-ALL-PUBLISHERS", "T-PLAN-RACE", "T-EXACT-SLOTS",
    "T-BYPASS-FAILURE", "T-LOWER-PRIMER", "T-PRIMED-NOT-COMPLETE",
    "T-PRIMER-CAPACITY", "T-ACK-BOUNDARY", "T-REARM-REGRESSES", "T-TRACK-LOCAL",
    "T-OFFLINE-NO-LEAK", "T-G4-WITNESS", "T-STALE-SNAPSHOT-AUTHORITY",
    "T-STABLE-DEVICE-TARGETS", "T-ROUTING-ATOMICITY", "T-DEVICE-ID-LIFETIME",
    "T-DUPLICATE-PLUGIN-MIRRORS", "T-CAPACITY-PERMANENT", "T-RECEIVER-BOUND",
    "T-MASTER-CORRELATION", "T-OFFLINE-PHASES", "T-MAPPING-PREFLIGHT",
    "T-PROTOCOL-VERSIONS", "T-BATCH-VISIBILITY", "T-PROJECT-TARGET-MIGRATION",
}

MUTATION_SCAN_RE = re.compile(
    r"(?:addDevice|removeDeviceById|moveDeviceById|setDevice[A-Za-z]+)\([^;]*track\.chain"
    r"|track\.chain\s*="
    r"|resetTrackContent\("
    r"|for \(auto& [^:]+:\s*[^)]*track\.chain\.devices"
    r"|auto& devices\s*=\s*[^;]*track\.chain\.devices"
    r"|track\.chain\.devices\.push_back\("
    r"|auto& devices\s*=\s*[^;]*chain\.devices"
    r"|target->patcher\s*="
)
AUTHORITY_SCAN_RE = re.compile(r"chainDevices|resolveDevicePluginPath")
ROUTING_SCAN_RE = re.compile(r"(?:\.|->)routing\b|routesToMaster")
MIRROR_SCAN_RE = re.compile(r"paramMirror|ParamMirrorEntry|ParamKeyLess")
DEVICE_ID_SCAN_RE = re.compile(
    r"kDeviceIdAuto|nextDeviceId|nextLoadedDeviceId|device\.id|device_id"
)
TARGET_ID_SCAN_RE = re.compile(r"targetPluginIndex|target_plugin_index|kParamTargetAll")
HOST_CONFIG_SCAN_RE = re.compile(
    r"config\.pluginPaths|config\.pluginNames|HostController::launch\("
    r"|HostController::spawnHostProcess\("
)
PROCESS_BLOCK_SEND_RE = re.compile(r"(?:->|\.)controller\.sendProcessBlock\(")
REPLAY_ORDER_RE = re.compile(r"eventPriority|EventType::Param|EventType::ReplayComplete")
OFFLINE_ENTRYPOINT_RE = re.compile(r"\brunOfflinePump\s*\(")

DEVICE_ID_SCAN_PATHS = {
    "apps/device_chain.h", "apps/device_chain.cpp", "apps/engine_chain_commands.cpp",
    "apps/project_file.cpp",
}


class Refused(RuntimeError):
    pass


def refuse(condition: bool, message: str) -> None:
    if condition:
        raise Refused(message)


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=ROOT)


def object_tree(commit: str) -> str:
    return git("show", "-s", "--format=%T", commit).decode().strip()


def safe_repo_path(path_text: str, authority: str) -> str:
    refuse(not path_text or "\\" in path_text, f"unsafe {authority} path: {path_text}")
    relative = Path(path_text)
    refuse(relative.is_absolute() or ".." in relative.parts or "." in relative.parts,
           f"unsafe {authority} path: {path_text}")
    refuse(relative.as_posix() != path_text, f"non-canonical {authority} path: {path_text}")
    return path_text


def frozen_text(path: str) -> str:
    commit = EXPECTED_EXTERNAL["frozen_product"]["commit"]
    return git("show", f"{commit}:{path}").decode()


def production_sources(commit: str) -> list[str]:
    paths = git("ls-tree", "-r", "--name-only", commit, "--", "apps").decode().splitlines()
    return sorted(
        path for path in paths
        if path.endswith((".cpp", ".h")) and "_tests_main.cpp" not in path
    )


def derive_line_scan(
    commit: str,
    pattern: re.Pattern[str],
    *,
    only_paths: set[str] | None = None,
    skip_line_comments: bool = False,
) -> tuple[tuple[str, int], ...]:
    found: list[tuple[str, int]] = []
    for path in production_sources(commit):
        if only_paths is not None and path not in only_paths:
            continue
        source = git("show", f"{commit}:{path}").decode()
        for line_no, line in enumerate(source.splitlines(), 1):
            if skip_line_comments and line.lstrip().startswith("//"):
                continue
            if pattern.search(line):
                found.append((path, line_no))
    return tuple(sorted(found))


def derive_mutation_scan(commit: str) -> tuple[tuple[str, int], ...]:
    cpp_paths = {path for path in production_sources(commit) if path.endswith(".cpp")}
    return derive_line_scan(commit, MUTATION_SCAN_RE, only_paths=cpp_paths)


def population_pairs(manifest: dict, name: str) -> list[tuple[str, int]]:
    return sorted((entry["path"], entry["line"]) for entry in manifest["populations"][name])


def resolve_manifest_pointer(manifest: dict, locator: str) -> object:
    refuse(not locator.startswith("manifest:/"), f"unparseable manifest locator: {locator}")
    value: object = manifest
    for raw in locator[len("manifest:/"):].split("/"):
        token = raw.replace("~1", "/").replace("~0", "~")
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


def validate_locator(manifest: dict, locator: str, governed_paths: set[str]) -> None:
    if locator.startswith("manifest:/"):
        resolve_manifest_pointer(manifest, locator)
        return
    if locator.startswith("packet:"):
        resolve_packet_path(locator)
        return
    match = re.fullmatch(r"(frozen|predecessor|item15):([^:]+):(\d+)-(\d+)", locator)
    refuse(match is None, f"unparseable source locator: {locator}")
    authority, path_text, first_text, last_text = match.groups()
    path = safe_repo_path(path_text, authority)
    first, last = int(first_text), int(last_text)
    refuse(first < 1 or last < first, f"invalid source range: {locator}")
    commits = {
        "frozen": EXPECTED_EXTERNAL["frozen_product"]["commit"],
        "predecessor": EXPECTED_EXTERNAL["predecessor"]["packet_commit"],
        "item15": EXPECTED_EXTERNAL["item15"]["packet_commit"],
    }
    lines = git("show", f"{commits[authority]}:{path}").decode().splitlines()
    refuse(last > len(lines), f"source range exceeds file: {locator}")
    if authority == "frozen":
        refuse(path not in governed_paths, f"ungoverned frozen locator: {locator}")


def render(manifest: dict) -> str:
    p = manifest["populations"]
    auth = manifest["implementation_authorization"]
    lines = [
        "# AE-P1.2 G2-B — item 18 readiness successor",
        "",
        "> Generated from AE-P1.2-g2b-item18-manifest.json; do not edit by hand.",
        "",
        f"Status: {manifest['status']}. Owner: {manifest['owner']}.",
        f"Frozen product: {manifest['frozen_product']['commit']} "
        f"(tree {manifest['frozen_product']['tree']}).",
        f"Item-15 input: {manifest['item15']['packet_commit']} / "
        f"manifest {manifest['item15']['manifest_sha256']}.",
        f"Revision successor to: {manifest['revision_predecessor']['packet_commit']} / "
        f"manifest {manifest['revision_predecessor']['manifest_sha256']}.",
        "",
        "## Scope",
        "",
        manifest["scope"],
        "",
        "Implementation authorized before dual PASS: "
        + str(auth["before_dual_pass"]).lower() + ".",
        "Implementation authorized after same-SHA semantic and evidence PASS: "
        + str(auth["after_same_sha_semantic_and_evidence_pass"]).lower() + ".",
        "Authorized scope: " + auth["scope"],
        "",
        "## Frozen populations",
        "",
        f"- Governed files: {len(manifest['governed_files'])}.",
        f"- Readiness publishers: {len(p['host_ready_true_sites'])}.",
        f"- TrackStateSnapshot publications: {len(p['track_snapshot_publications'])}.",
        f"- Mutation lexical candidates: {len(p['chain_mutation_scan'])}.",
        f"- Chain/path authority lexical candidates: "
        f"{len(p['execution_authority_lexical_scan'])}.",
        f"- Routing authority lexical candidates: {len(p['routing_authority_lexical_scan'])}.",
        f"- Mirror identity lexical candidates: {len(p['mirror_identity_lexical_scan'])}.",
        f"- Device identity lexical candidates: {len(p['device_identity_lexical_scan'])}.",
        f"- Target identity lexical candidates: {len(p['target_identity_lexical_scan'])}.",
        f"- Host-config lexical candidates: {len(p['host_config_lexical_scan'])}.",
        f"- Semantic execution consumers: {len(p['execution_authority_consumers'])}.",
        f"- Document-only exemptions: {len(p['authored_document_exemptions'])}.",
        f"- ProcessBlock send expressions: {len(p['process_block_senders'])}.",
        f"- Replay protocol sites: {len(p['replay_protocol_sites'])}; "
        f"receiver-order sites: {len(p['replay_receiver_order_sites'])}.",
        f"- Offline coordinator sites: {len(p['offline_coordinator_sites'])}.",
        "",
        "## Review history",
        "",
    ]
    for review in manifest["review_history"]:
        lines.append(
            f"- {review['packet_commit']}: semantic {review['semantic']}, "
            f"evidence {review['evidence']}. {review['resolution']}"
        )
    lines.extend(["", "## Records", ""])
    for record in manifest["records"]:
        lines.append(
            f"- {record['id']} [{record['kind']} / {record['status']}]: "
            + record["statement"]
        )
    lines.extend(["", "## Required implementation tests", ""])
    for case in manifest["test_cases"]:
        lines.append(f"- {case['id']}: {case['statement']}")
    lines.extend(["", "## Non-goals", ""])
    lines.extend(f"- {item}" for item in manifest["non_goals"])
    return "\n".join(lines) + "\n"


def validate_exact_sections(manifest: dict) -> None:
    refuse(set(manifest) != TOP_LEVEL_KEYS, "top-level manifest shape changed")
    for key in sorted(TOP_LEVEL_KEYS):
        observed = sha256(canonical(manifest[key]))
        refuse(observed != EXPECTED_SECTION_DIGESTS[key], f"canonical section changed: {key}")


def validate_graph_and_semantics(manifest: dict) -> None:
    records = manifest["records"]
    tests = manifest["test_cases"]
    ids = [record.get("id") for record in records]
    test_ids = [case.get("id") for case in tests]
    refuse(len(ids) != len(set(ids)) or set(ids) != EXPECTED_RECORD_IDS,
           "record id population changed")
    refuse(set(manifest["changed_records"]) != EXPECTED_RECORD_IDS,
           "schema-v4 changed-record closure is incomplete")
    refuse(len(test_ids) != len(set(test_ids)) or set(test_ids) != EXPECTED_TEST_IDS,
           "test id population changed")
    by_id = {record["id"]: record for record in records}
    for record in records:
        deps = record.get("dependencies")
        refuse(not isinstance(deps, list), f"dependencies are not a list: {record['id']}")
        for dep in deps:
            refuse(dep not in by_id, f"dangling dependency {dep} from {record['id']}")

    visiting: set[str] = set()
    visited: set[str] = set()
    def visit(record_id: str) -> None:
        refuse(record_id in visiting, f"dependency cycle at {record_id}")
        if record_id in visited:
            return
        visiting.add(record_id)
        for dep in by_id[record_id]["dependencies"]:
            visit(dep)
        visiting.remove(record_id)
        visited.add(record_id)
    for record_id in by_id:
        visit(record_id)

    closure: set[str] = set()
    stack = ["R-REVIEW-GATED-AUTH"]
    while stack:
        current = stack.pop()
        if current in closure:
            continue
        closure.add(current)
        stack.extend(by_id[current]["dependencies"])
    required = {
        "G-ITEM18", "R-HOST-PLAN-AUTHORITY", "R-ROUTING-AUTHORITY",
        "R-DEVICE-ID-LIFETIME", "R-STABLE-DEVICE-TARGETS",
        "R-MIRROR-INSTANCE-IDENTITY", "R-PROTOCOL-VERSION",
        "D-PRODUCTION-RECEIVER", "R-PROJECT-TARGET-MIGRATION",
    }
    refuse(not required.issubset(closure), "authorization dependency closure is incomplete")

    auth = manifest["implementation_authorization"]
    refuse(auth["before_dual_pass"] is not False, "implementation authorized before dual PASS")
    refuse(auth["after_same_sha_semantic_and_evidence_pass"] is not True,
           "same-SHA dual PASS no longer authorizes declared scope")

    authority = by_id["R-HOST-PLAN-AUTHORITY"]["statement"]
    for token in (
        "session ExecutionSnapshot", "one atomic snapshot publication", "HostConfig plugin vectors",
        "TrackStateSnapshot.chainDevices", "TrackStateSnapshot.routing", "routesToMaster",
        "revision exhaustion",
    ):
        refuse(token not in authority, f"session authority weakened: missing {token}")
    routing = by_id["R-ROUTING-AUTHORITY"]["statement"]
    for token in ("one session ExecutionSnapshot revision", "routesToMaster atomic are removed",
                  "no old/new route combination"):
        refuse(token not in routing, f"routing authority weakened: missing {token}")
    device_ids = by_id["R-DEVICE-ID-LIFETIME"]["statement"]
    for token in ("[1, UINT32_MAX-1]", "next_device_id", "never reuses", "exhaustion",
                  "duplicate", "fail project load"):
        refuse(token not in device_ids, f"device-id lifetime weakened: missing {token}")
    mirror = by_id["R-MIRROR-INSTANCE-IDENTITY"]["statement"]
    for token in ("{stableDeviceId, parameterUid16}", "all-target", "one mirror entry per concrete",
                  "distinct compact index"):
        refuse(token not in mirror, f"mirror instance identity weakened: missing {token}")
    ack = by_id["R-CORRELATED-REPLAY-ACK"]["statement"]
    for token in ("ReplayCompletePayload", "max(lastIssuedGate, liveAck)+1", "UINT64_MAX",
                  "strictly below"):
        refuse(token not in ack, f"correlated acknowledgement weakened: missing {token}")
    capacity = by_id["R-ATOMIC-PRIMER-CAPACITY"]["statement"]
    for token in ("ringWriteBatch", "one CAS", "ready=0", "first reserved ready flag last",
                  "C-2", "N>=C-1", "Every batch result is consumed"):
        refuse(token not in capacity, f"capacity boundary weakened: missing {token}")
    master = by_id["R-MASTER-CORRELATION"]["statement"]
    for token in ("hostGeneration", "dispatchTicket", "blockId",
                  "Boolean hostReady/masterFxActive cannot authorize"):
        refuse(token not in master, f"master correlation weakened: missing {token}")
    offline = by_id["R-OFFLINE-PRIMER"]["statement"]
    for token in ("ControlPreroll", "transport is stopped", "matching track and master",
                  "renderFailed"):
        refuse(token not in offline, f"offline phase closure weakened: missing {token}")
    project = by_id["R-PROJECT-TARGET-MIGRATION"]["statement"]
    for token in ("schema advances 4 to 5", "target_device_id", "next_device_id",
                  "every concrete legacy", "unknowable", "disables the lane",
                  "never mapped through the current machine", "Schema 5 rejects"):
        refuse(token not in project, f"project migration weakened: missing {token}")
    receiver = by_id["D-PRODUCTION-RECEIVER"]["statement"]
    for token in ("used by handleProcessBlock", "eventPriority", "Param precedes ReplayComplete",
                  "actual application", "runControlLoop"):
        refuse(token not in receiver, f"production receiver binding weakened: missing {token}")
    protocol = by_id["R-PROTOCOL-VERSION"]["statement"]
    refuse("kControlVersion is 15" not in protocol or "kShmVersion is 42" not in protocol,
           "protocol semantic version bump removed")
    offline_population = by_id["P-OFFLINE-OUTPUT-SURFACES"]["statement"]
    refuse("daw_engine_main.cpp:2127" not in offline_population,
           "real offline coordinator call is not pinned")


def validate_population_lines(manifest: dict, governed_paths: set[str]) -> None:
    no_line = {"classified_non_host_chain_mutations"}
    for name, entries in manifest["populations"].items():
        refuse(not isinstance(entries, list), f"population is not a list: {name}")
        refuse(len(entries) != EXPECTED_COUNTS[name], f"population count changed: {name}")
        for entry in entries:
            path = entry.get("path")
            refuse(not isinstance(path, str) or path not in governed_paths,
                   f"population path is malformed or ungoverned: {name}:{path}")
            if name in no_line:
                continue
            line = entry.get("line")
            refuse(not isinstance(line, int), f"population line is malformed: {name}")
            lines = frozen_text(path).splitlines()
            refuse(line < 1 or line > len(lines), f"population line is invalid: {path}:{line}")

    offline_tokens = {
        "coordinator_invocation": "runOfflinePump",
        "coordinator_definition": "runOfflinePump",
        "coordinator_declaration": "runOfflinePump",
        "mapping_preflight": "awaitAnyReadyTrack",
        "timeline_reset": "resetTimeline.store",
        "producer_arm": "offlineProducerArmed.store",
        "production_preflight": "awaitAllReadyTracks",
        "counted_block_wait": "awaitNextBlock",
        "counted_mix": "audioCallback->process",
        "count_increment": "++rendered",
        "output_writer": "writeWav16",
        "arm_observer": "offlineProducerArmed",
    }
    for entry in manifest["populations"]["offline_coordinator_sites"]:
        line = frozen_text(entry["path"]).splitlines()[entry["line"] - 1]
        token = offline_tokens[entry["role"]]
        refuse(token not in line, f"offline role locator does not name {token}: {entry}")

    replay_tokens = {
        "parameter_sender": "ringWrite",
        "gate_sender": "ringWrite",
        "prime_publication": "mirrorPrimed.store",
        "gate_consumer": "ReplayComplete",
        "ack_high_water_read": "replayAckSampleTime.load",
        "ack_publisher": "replayAckSampleTime.store",
        "ack_reader": "replayAckSampleTime.load",
        "ack_word": "replayAckSampleTime",
    }
    for entry in manifest["populations"]["replay_protocol_sites"]:
        line = frozen_text(entry["path"]).splitlines()[entry["line"] - 1]
        token = replay_tokens[entry["role"]]
        refuse(token not in line, f"replay role locator does not name {token}: {entry}")


def validate_frozen_evidence(manifest: dict) -> None:
    frozen = EXPECTED_EXTERNAL["frozen_product"]
    refuse(object_tree(frozen["commit"]) != frozen["tree"], "frozen product tree mismatch")
    for key in ("predecessor", "item15", "revision_predecessor", "program_source"):
        identity = EXPECTED_EXTERNAL[key]
        commit_key = "commit" if key == "program_source" else "packet_commit"
        tree_key = "tree" if key == "program_source" else "packet_tree"
        refuse(object_tree(identity[commit_key]) != identity[tree_key], f"{key} tree mismatch")

    predecessor = EXPECTED_EXTERNAL["predecessor"]
    pred_bytes = git("show", f"{predecessor['packet_commit']}:{PREDECESSOR_MANIFEST}")
    refuse(sha256(pred_bytes) != predecessor["manifest_sha256"],
           "predecessor manifest digest mismatch")
    item15 = EXPECTED_EXTERNAL["item15"]
    item15_bytes = git("show", f"{item15['packet_commit']}:{ITEM15_MANIFEST}")
    refuse(sha256(item15_bytes) != item15["manifest_sha256"],
           "item-15 manifest digest mismatch")
    revision = EXPECTED_EXTERNAL["revision_predecessor"]
    rel_manifest = MANIFEST_PATH.relative_to(ROOT)
    revision_bytes = git("show", f"{revision['packet_commit']}:{rel_manifest}")
    refuse(sha256(revision_bytes) != revision["manifest_sha256"],
           "revision predecessor manifest digest mismatch")

    governed = manifest["governed_files"]
    refuse(len(governed) != EXPECTED_COUNTS["governed_files"], "governed file count changed")
    paths = [safe_repo_path(entry["path"], "governed") for entry in governed]
    refuse(len(paths) != len(set(paths)), "duplicate governed path")
    for entry in governed:
        path = entry["path"]
        blob = git("show", f"{frozen['commit']}:{path}")
        refuse(sha256(blob) != entry["sha256"], f"frozen blob mismatch: {path}")
        checkout = ROOT / path
        refuse(not checkout.is_file(), f"missing governed checkout file: {path}")
        refuse(sha256(checkout.read_bytes()) != entry["sha256"], f"checkout drift: {path}")

    raw_ready = git("grep", "-n", "-F", "hostReady.store(true", frozen["commit"], "--", "apps")
    observed_ready: list[tuple[str, int]] = []
    for raw in raw_ready.decode().splitlines():
        if "_tests_main" in raw:
            continue
        match = re.fullmatch(r"[^:]+:([^:]+):(\d+):.*", raw)
        refuse(match is None, f"unparseable readiness grep line: {raw}")
        observed_ready.append((match.group(1), int(match.group(2))))
    refuse(sorted(observed_ready) != population_pairs(manifest, "host_ready_true_sites"),
           "readiness publisher census drifted")

    observed_snapshots: list[tuple[str, int, str]] = []
    for path in production_sources(frozen["commit"]):
        source = frozen_text(path)
        for match in re.finditer(
            r"(?:std::)?atomic_store_explicit\s*\(\s*&[A-Za-z_][A-Za-z0-9_]*->trackSnapshot",
            source,
        ):
            observed_snapshots.append((path, source.count("\n", 0, match.start()) + 1, "atomic"))
        for match in re.finditer(
            r"[A-Za-z_][A-Za-z0-9_]*->trackSnapshot\s*=\s*buildTrackSnapshot", source
        ):
            observed_snapshots.append(
                (path, source.count("\n", 0, match.start()) + 1, "plain_prepublication")
            )
    expected_snapshots = sorted(
        (
            entry["path"], entry["line"],
            "atomic" if entry["kind"] == "atomic_unlocked_build" else entry["kind"],
        )
        for entry in manifest["populations"]["track_snapshot_publications"]
    )
    refuse(sorted(observed_snapshots) != expected_snapshots,
           "TrackStateSnapshot publication census drifted")

    derived_scans = {
        "chain_mutation_scan": derive_mutation_scan(frozen["commit"]),
        "execution_authority_lexical_scan": derive_line_scan(
            frozen["commit"], AUTHORITY_SCAN_RE
        ),
        "routing_authority_lexical_scan": derive_line_scan(frozen["commit"], ROUTING_SCAN_RE),
        "mirror_identity_lexical_scan": derive_line_scan(
            frozen["commit"], MIRROR_SCAN_RE, skip_line_comments=True
        ),
        "device_identity_lexical_scan": derive_line_scan(
            frozen["commit"], DEVICE_ID_SCAN_RE, only_paths=DEVICE_ID_SCAN_PATHS,
            skip_line_comments=True,
        ),
        "target_identity_lexical_scan": derive_line_scan(
            frozen["commit"], TARGET_ID_SCAN_RE, skip_line_comments=True
        ),
        "host_config_lexical_scan": derive_line_scan(frozen["commit"], HOST_CONFIG_SCAN_RE),
        "process_block_senders": derive_line_scan(frozen["commit"], PROCESS_BLOCK_SEND_RE),
        "replay_receiver_order_sites": derive_line_scan(
            frozen["commit"], REPLAY_ORDER_RE,
            only_paths={"apps/juce_host_process_main.cpp"},
        ),
    }
    for name, observed in derived_scans.items():
        refuse(list(observed) != population_pairs(manifest, name), f"derived census drifted: {name}")

    offline_entrypoints = derive_line_scan(frozen["commit"], OFFLINE_ENTRYPOINT_RE)
    expected_entrypoints = sorted(
        (entry["path"], entry["line"])
        for entry in manifest["populations"]["offline_coordinator_sites"]
        if entry["role"].startswith("coordinator_")
    )
    refuse(list(offline_entrypoints) != expected_entrypoints,
           "offline runOfflinePump entrypoint census drifted")


def validate(
    manifest: dict,
    *,
    verify_files: bool = True,
    verify_prose: bool = True,
) -> None:
    validate_exact_sections(manifest)
    for key, identity in EXPECTED_EXTERNAL.items():
        refuse(manifest[key] != identity, f"external identity changed: {key}")
    refuse(manifest["schema"] != "ae-p1.2-g2b-item18-packet/4", "schema is not v4")
    refuse(len(manifest["records"]) != EXPECTED_COUNTS["records"], "record count changed")
    refuse(len(manifest["test_cases"]) != EXPECTED_COUNTS["test_cases"], "test count changed")
    validate_graph_and_semantics(manifest)

    governed_paths = {entry["path"] for entry in manifest["governed_files"]}
    validate_population_lines(manifest, governed_paths)
    for record in manifest["records"]:
        spans = record.get("source_span")
        spans = spans if isinstance(spans, list) else [spans]
        refuse(any(not isinstance(span, str) for span in spans),
               f"invalid source span shape: {record['id']}")
        for locator in spans:
            validate_locator(manifest, locator, governed_paths)

    if verify_files:
        validate_frozen_evidence(manifest)
    if verify_prose:
        refuse(not PROSE_PATH.is_file(), "generated prose is missing")
        refuse(PROSE_PATH.read_text(encoding="utf-8") != render(manifest),
               "generated prose differs from manifest")


def self_test(manifest: dict) -> int:
    # Scanner controls are synthetic so the two schema-v3 omissions cannot regress while the
    # frozen product remains unchanged.
    scanner_fixtures = {
        "direct project chain mutation": (MUTATION_SCAN_RE, "track.chain.devices.push_back(device);"),
        "chain alias declaration": (
            MUTATION_SCAN_RE, "auto& devices = document.tracks.front().chain.devices;"
        ),
        "alias sink": (MUTATION_SCAN_RE, "target->patcher = patcherGraphState.graph;"),
        "routing alias": (ROUTING_SCAN_RE, "runtime->routesToMaster.store(true);"),
        "mirror key": (MIRROR_SCAN_RE, "runtime.paramMirror[key] = value;"),
        "device watermark input": (DEVICE_ID_SCAN_RE, "uint32_t nextDeviceId = 1;"),
        "durable target identity": (TARGET_ID_SCAN_RE, "clip.targetPluginIndex();"),
        "host config consumer": (HOST_CONFIG_SCAN_RE, "runtime.config.pluginPaths = paths;"),
        "receiver priority": (REPLAY_ORDER_RE, "return EventType::Param;"),
        "offline call": (OFFLINE_ENTRYPOINT_RE, "daw::engine::runOfflinePump(deps);"),
    }
    for name, (pattern, text) in scanner_fixtures.items():
        refuse(pattern.search(text) is None, f"scanner fixture missed: {name}")

    cases: list[tuple[str, dict]] = []
    def add(name: str, candidate: dict) -> None:
        cases.append((name, candidate))

    for key in sorted(TOP_LEVEL_KEYS):
        candidate = copy.deepcopy(manifest)
        candidate.pop(key)
        add(f"top-level deletion: {key}", candidate)

    candidate = copy.deepcopy(manifest)
    candidate["implementation_authorization"]["before_dual_pass"] = True
    add("premature authorization", candidate)
    candidate = copy.deepcopy(manifest)
    candidate["revision_predecessor"]["packet_commit"] = "0" * 40
    add("revision identity substitution", candidate)
    candidate = copy.deepcopy(manifest)
    candidate["governed_files"][0]["sha256"] = "0" * 64
    add("governed digest substitution", candidate)

    for population in sorted(manifest["populations"]):
        candidate = copy.deepcopy(manifest)
        candidate["populations"][population].pop()
        add(f"population deletion: {population}", candidate)

    for record_id in (
        "R-HOST-PLAN-AUTHORITY", "R-ROUTING-AUTHORITY", "R-DEVICE-ID-LIFETIME",
        "R-STABLE-DEVICE-TARGETS", "R-MIRROR-INSTANCE-IDENTITY", "R-DISPATCH-TICKET",
        "R-MIRROR-EPOCH", "R-CORRELATED-REPLAY-ACK", "R-ATOMIC-PRIMER-CAPACITY",
        "R-PASS4-REPLACEMENT", "R-MASTER-CORRELATION", "R-OFFLINE-PRIMER",
        "R-PROTOCOL-VERSION", "R-PROJECT-TARGET-MIGRATION", "D-PRODUCTION-RECEIVER",
    ):
        candidate = copy.deepcopy(manifest)
        next(record for record in candidate["records"] if record["id"] == record_id)[
            "statement"
        ] = "weakened"
        add(f"record weakening: {record_id}", candidate)

    for test_id in (
        "T-PLAN-RACE", "T-ROUTING-ATOMICITY", "T-DEVICE-ID-LIFETIME",
        "T-DUPLICATE-PLUGIN-MIRRORS", "T-ACK-BOUNDARY", "T-CAPACITY-PERMANENT",
        "T-RECEIVER-BOUND", "T-MASTER-CORRELATION", "T-OFFLINE-PHASES",
        "T-MAPPING-PREFLIGHT", "T-PROTOCOL-VERSIONS", "T-BATCH-VISIBILITY",
        "T-PROJECT-TARGET-MIGRATION",
    ):
        candidate = copy.deepcopy(manifest)
        next(case for case in candidate["test_cases"] if case["id"] == test_id)[
            "statement"
        ] = "eventually succeeds"
        add(f"test weakening: {test_id}", candidate)

    refused = 0
    for name, candidate in cases:
        try:
            validate(candidate, verify_files=False, verify_prose=False)
        except (Refused, KeyError):
            refused += 1
        else:
            raise Refused(f"mutation was accepted: {name}")
    refuse(refused < 65, f"too few mutation controls: {refused}")
    return refused


def main() -> int:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    if "--render" in sys.argv:
        print(render(manifest), end="")
        return 0
    validate(manifest)
    controls = self_test(manifest)
    p = manifest["populations"]
    print("AE-P1.2 G2-B item 18 schema-v4 packet: PASS")
    print(f"  records: {len(manifest['records'])}")
    print(f"  tests: {len(manifest['test_cases'])}")
    print(f"  governed files: {len(manifest['governed_files'])}")
    print(f"  readiness publishers: {len(p['host_ready_true_sites'])}")
    print(f"  snapshot publications: {len(p['track_snapshot_publications'])}")
    print(f"  mutation candidates: {len(p['chain_mutation_scan'])}")
    print(f"  chain/path authority candidates: {len(p['execution_authority_lexical_scan'])}")
    print(f"  routing candidates: {len(p['routing_authority_lexical_scan'])}")
    print(f"  mirror identity candidates: {len(p['mirror_identity_lexical_scan'])}")
    print(f"  device identity candidates: {len(p['device_identity_lexical_scan'])}")
    print(f"  target identity candidates: {len(p['target_identity_lexical_scan'])}")
    print(f"  host-config candidates: {len(p['host_config_lexical_scan'])}")
    print(f"  semantic consumers: {len(p['execution_authority_consumers'])}")
    print(f"  ProcessBlock senders: {len(p['process_block_senders'])}")
    print(f"  mutation controls: {controls}/{controls} refused")
    print("  implementation before dual PASS: false")
    print("  implementation after same-SHA dual PASS: true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Refused, subprocess.CalledProcessError, UnicodeDecodeError,
            json.JSONDecodeError, KeyError) as exc:
        print(f"AE-P1.2 G2-B item 18 packet: REFUSED: {exc}", file=sys.stderr)
        raise SystemExit(1)
