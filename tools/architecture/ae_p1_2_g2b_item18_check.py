#!/usr/bin/env python3
"""Structural and exact-evidence checker for AE-P1.2 G2-B item-18 schema v9."""

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
    "routing_matrix", "artifact_presence_matrix", "changed_records", "records", "test_cases",
}

EXPECTED_EXTERNAL = {
    "revision_predecessor": {
        "packet_commit": "e3dbe2a37cb7dbf6e267a9066db33aea41e56306",
        "packet_tree": "4d5393fe9c91f85da9c7889c8222dac81fec56db",
        "manifest_sha256": "1dd5ce18c66c576ffce3404647bdced7bab34435856045ba79ad1c838ed4e0dc",
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
    "schema": "932fe6664c3414facfdbc0e0568550666d1eeff7cdd086da7d20f2fc93b08ae4",
    "ticket": "51fc055b4f8c131bfb5f08b4d03e9141b5667cc3f398ef763016462cf9fd1380",
    "status": "a9214bd949881802e074114cc9ffc50cbf88f003ee4a269801e41dd13fdecf5c",
    "owner": "74a80ba7942f31913df8aed6e77553465be5b3c80bc5666036f42a54a94c828a",
    "revision_predecessor": "b020c02bec1c9106522d1020e40fa0915e8339148a92b573a2699d31f0ab24a1",
    "review_history": "97de41c4152898f1a65441968bf2078bcaf4c7ced7606a0170f1096a07de9a80",
    "predecessor": "a87219ba250bdbb90663db22afac5052bc7379e485789cc40e10abc21e8f5959",
    "item15": "0bb1bb2ed7ab992ab7607c8fb4517a26b6bdc7698e8836b8ba3e57bc33884c32",
    "program_source": "2cbd44acb7ae764d5b84e8bcb76cc3f34dd435e4d734104809d199fbb76fa212",
    "frozen_product": "4dcff51b0d360b6c2ebafb0ab2b44f94ada495a53144f15cf6ad5e220ae856de",
    "routing_matrix": "67876cfdb7be392f00c1d132fef2ec4291e4ea23f2b260b4f9d7a5f745e9011d",
    "artifact_presence_matrix": "0796298e026a8803032234ceeb4be8cdb3e2f78cd995724b54d06646518582e9",
    "scope": "2322538f20c98ad469414f890b8010fbbc67b48c2af0907c1e9044900e654090",
    "implementation_authorization": "8a02dbbd39a3134fcc3256b264bd617e2c162bc950cc72cf89ebd2a960eeaa8e",
    "non_goals": "29742d1034c4651dde966d23e504d5193edc68175a8a54692a43e4b6b9cdc546",
    "governed_files": "6ea1f98092b8f8ba1dc55cac0dbdc21f2354ae0172a1f89acfd9782021d609f0",
    "populations": "ba32eae4e2b2ba9c6f648c866ca169c202ba7bc76500c80a85b994bbde0b34ed",
    "changed_records": "2dce85a326c8f654eeae78ff4501f6855bca6eb708211892c256fc63283cc5d1",
    "records": "000948105c51538976acf7e916bac5f939d0f6a00a20d439a3e83d7e55792335",
    "test_cases": "6b5d67bb46fe2b1973897db948e5c648c3b2a65a2afaf60d243927ae0d50ebe3",
}

EXPECTED_COUNTS = {
    "governed_files": 89,
    "records": 33,
    "test_cases": 39,
    "host_ready_true_sites": 3,
    "track_snapshot_publications": 24,
    "host_plan_mutation_roots": 21,
    "classified_non_host_chain_mutations": 4,
    "chain_mutation_scan": 67,
    "execution_authority_lexical_scan": 57,
    "routing_authority_lexical_scan": 51,
    "mirror_identity_lexical_scan": 26,
    "device_identity_lexical_scan": 25,
    "stable_device_carrier_sites": 25,
    "target_identity_lexical_scan": 41,
    "host_config_lexical_scan": 28,
    "execution_authority_consumers": 81,
    "authored_document_exemptions": 13,
    "process_block_senders": 3,
    "process_block_receiver": 2,
    "replay_protocol_sites": 9,
    "replay_lifecycle_lexical_scan": 36,
    "replay_receiver_order_sites": 7,
    "offline_coordinator_sites": 12,
    "mapping_and_output_gates": 100,
    "capacity_contract_sites": 5,
    "event_ring_write_sites": 8,
    "document_restore_and_identity_sites": 11,
    "state_artifact_identity_sites": 17,
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
    "R-TRANSACTIONAL-EVENT-BATCH", "R-ATOMIC-PRIMER-CAPACITY",
    "R-MASTER-CORRELATION", "R-PROTOCOL-VERSION",
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
    "T-ORDINARY-BATCH-ATOMIC", "T-NO-DRAIN-LEAK", "T-PRODUCER-GATES",
    "T-GLOBAL-DEVICE-IDENTITY", "T-SESSION-BLOCK-ATOMIC",
    "T-LEGACY-DISABLED-ROUNDTRIP", "T-ALL-EVENT-WRITERS",
    "T-STATE-ARTIFACT-MIGRATION", "T-ROUTING-BLOCK-DETERMINISM",
    "T-ORDINARY-CAPACITY-PERMANENT",
    "T-ROUTING-MATRIX", "T-ARTIFACT-PRESENCE-MATRIX",
    "T-ARTIFACT-PROVENANCE",
}

EXPECTED_MUTATION_CONTROLS = 118

MUTATION_SCAN_RE = re.compile(
    r"(?:addDevice|removeDeviceById|moveDeviceById|setDevice[A-Za-z]+)\([^;]*(?:->|\.)chain"
    r"|(?:->|\.)chain\s*="
    r"|resetTrackContent\("
    r"|for \(auto& [^:]+:\s*[^)]*(?:->|\.)chain\.devices"
    r"|auto&\s+\w+\s*=\s*[^;]*chain\.devices"
    r"|(?:->|\.)chain\.devices\.push_back\("
    r"|\w+(?:->|\.)patcher\s*="
    r"|\w+(?:->|\.)patcherNodeId\s*="
    r"|\b\w+\.applyDocument\s*&&\s*\w+\.applyDocument\("
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
REPLAY_LIFECYCLE_RE = re.compile(
    r"mirror(?:Pending|Primed|GateSampleTime|Causes)|ReplayComplete|replayAckSampleTime"
)
EVENT_RING_WRITE_RE = re.compile(r"\bdaw::ringWrite\s*\(")
MAPPING_GATE_BASE_RE = re.compile(
    r"\b(?:hostReady|active|completedBlockId|m_masterHostReady|m_masterFxActive|"
    r"mirrorPending|mirrorPrimed)\b[^;\n]*(?:->|\.)\s*load\s*\("
    r"|\b(?:mappingIsCurrent|readinessLevel|mirrorReplayCanComplete)\s*\("
    r"|(?:->|\.)sendProcessBlock\s*\("
    r"|EngineAudioCallback::TrackInfo\s+[A-Za-z_]\w*"
    r"|bool\s+await(?:Any|All)ReadyTracks?\s*\("
)
DOCUMENT_IDENTITY_RE = re.compile(
    r"[A-Za-z_]\w*\.(?:undo|redo)\(|(?:->|\.)applyDocument\s*\("
    r"|[A-Za-z_]\w*\.field\(\"(?:device_id|routing|chain|mod_links|devices)\""
    r"|uint32_t deviceId"
)
STATE_ARTIFACT_HELPER_RE = re.compile(
    r"plugin(?:State|Params)FileName|pluginStateDirFor\s*\(|moduleStatePrefix\s*\("
)
STATE_ARTIFACT_PACKAGE_RE = re.compile(
    r"std::filesystem::directory_iterator\s*\("
    r"|[A-Za-z_]\w*\.path\(\)\.filename\(\)\.string\(\)"
    r"|[A-Za-z_]\w*\.name\s*=\s*[A-Za-z_]\w*\s*\+\s*[A-Za-z_]\w*"
)
OFFLINE_ENTRYPOINT_RE = re.compile(r"\brunOfflinePump\s*\(")

CARRIER_SCAN_RE = re.compile(
    r"uint16_t deviceId|uint16_t ownerDeviceId|device_id:\s*u16|owner_device_id:\s*u16"
    r"|kUiPatcherDeviceIdMask|UI_PATCHER_DEVICE_ID_MASK|K_UI_PATCHER_DEVICE_ID_MASK"
    r"|packSamplerAddr|static_cast<uint16_t>\(n.ownerDeviceId\)"
    r"|ownerDeviceId\s*>\s*0xFFFF|deviceId\s*&\s*0xffff"
    r"|owner\s*&\s*0x7FFF|device\s*>\s*0x7FFF|\b(?:device|owner|id)\s+as\s+u16"
    r"|device_id\s*:[^\n]*as\s+u16|UiSamplerSlotNameHeader\s+[A-Za-z_]\w*\s*\{\}"
)

CARRIER_SCAN_PATHS = {
    "apps/event_payloads.h", "apps/shared_memory.h", "apps/engine_consumer.cpp",
    "apps/engine_patcher_commands.cpp", "apps/engine_request_commands.cpp",
    "apps/patcher_graph.h", "apps/engine_bulk_edit.cpp", "ui/daw-agent/src/tools.rs",
    "ui/daw-bridge/src/layout.rs", "ui/daw-cli/src/main.rs",
    "ui/daw-sidecar/src/main.rs",
}

DOCUMENT_IDENTITY_PATHS = {
    "apps/engine_undo_commands.cpp", "apps/document_visitor_fields.h", "apps/modulation.h",
}

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


def derive_named_paths_scan(
    commit: str,
    pattern: re.Pattern[str],
    paths: set[str],
    *,
    skip_line_comments: bool = False,
) -> tuple[tuple[str, int], ...]:
    found: list[tuple[str, int]] = []
    for path in sorted(paths):
        source = git("show", f"{commit}:{path}").decode()
        for line_no, line in enumerate(source.splitlines(), 1):
            if skip_line_comments and line.lstrip().startswith("//"):
                continue
            if pattern.search(line):
                found.append((path, line_no))
    return tuple(sorted(found))


def derive_mutation_scan(commit: str) -> tuple[tuple[str, int], ...]:
    cpp_paths = {path for path in production_sources(commit) if path.endswith(".cpp")}
    return derive_line_scan(
        commit, MUTATION_SCAN_RE, only_paths=cpp_paths, skip_line_comments=True
    )


def strip_cpp_line_comments(source: str) -> str:
    """Remove line comments while preserving line numbers for lexical evidence scans."""
    return "\n".join(line.split("//", 1)[0] for line in source.splitlines())


def derive_mapping_output_gates_from_sources(
    sources: dict[str, str],
) -> tuple[tuple[str, int], ...]:
    """Derive direct gates plus local aliases from stable member/type grammar.

    Local names are deliberately discovered from assignments to the stable master-FX,
    mirror-stage, and completed-block members. Renaming a receiver, witness, or local boolean
    therefore cannot silently shrink the census.
    """
    found: set[tuple[str, int]] = set()
    for path, raw_source in sorted(sources.items()):
        source = strip_cpp_line_comments(raw_source)
        lines = source.splitlines()
        aliases: set[str] = set()

        for match in re.finditer(
            r"auto\s*&\s*([A-Za-z_]\w*)\s*=\s*[^;]*\.masterFxActive\s*;", source,
        ):
            aliases.add(match.group(1))
        for match in re.finditer(
            r"(?:const\s+)?(?:auto|uint32_t)\s+([A-Za-z_]\w*)\s*=\s*"
            r"[^;]*completedBlockId[^;]*load\s*\([^;]*;",
            source,
            re.DOTALL,
        ):
            aliases.add(match.group(1))
        for match in re.finditer(
            r"\b([A-Za-z_]\w*)\s*=\s*[^;]*completedBlockId[^;]*load\s*\([^;]*;",
            source,
            re.DOTALL,
        ):
            aliases.add(match.group(1))
        for match in re.finditer(
            r"if\s*\([^{};]*mirrorPending[^{};]*load\s*\([^{};]*"
            r"mirrorPrimed[^{};]*load\s*\([^{};]*\)\s*\)\s*\{\s*"
            r"([A-Za-z_]\w*)\s*=\s*true\s*;",
            source,
            re.DOTALL,
        ):
            aliases.add(match.group(1))

        for line_no, line in enumerate(lines, 1):
            if MAPPING_GATE_BASE_RE.search(line):
                found.add((path, line_no))
            for alias in aliases:
                if re.search(rf"\b{re.escape(alias)}\b", line):
                    found.add((path, line_no))
                    break
    return tuple(sorted(found))


def derive_mapping_output_gates(commit: str) -> tuple[tuple[str, int], ...]:
    sources = {
        path: git("show", f"{commit}:{path}").decode()
        for path in production_sources(commit)
    }
    return derive_mapping_output_gates_from_sources(sources)


def derive_state_artifact_sites_from_sources(
    sources: dict[str, str],
) -> tuple[tuple[str, int], ...]:
    """Derive filename/directory helpers and the module package-name boundary."""
    found: set[tuple[str, int]] = set()
    for path, raw_source in sorted(sources.items()):
        source = strip_cpp_line_comments(raw_source)
        for line_no, line in enumerate(source.splitlines(), 1):
            if STATE_ARTIFACT_HELPER_RE.search(line):
                found.add((path, line_no))
            if path == "apps/project_file.cpp" and STATE_ARTIFACT_PACKAGE_RE.search(line):
                found.add((path, line_no))
    return tuple(sorted(found))


def derive_state_artifact_sites(commit: str) -> tuple[tuple[str, int], ...]:
    sources = {
        path: git("show", f"{commit}:{path}").decode()
        for path in production_sources(commit)
    }
    return derive_state_artifact_sites_from_sources(sources)


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
        f"- Routing decision rows: {len(manifest['routing_matrix']['rows'])}.",
        f"- Artifact presence rows: {len(manifest['artifact_presence_matrix']['rows'])}.",
        f"- Governed files: {len(manifest['governed_files'])}.",
        f"- Readiness publishers: {len(p['host_ready_true_sites'])}.",
        f"- TrackStateSnapshot publications: {len(p['track_snapshot_publications'])}.",
        f"- Mutation lexical candidates: {len(p['chain_mutation_scan'])}.",
        f"- Chain/path authority lexical candidates: "
        f"{len(p['execution_authority_lexical_scan'])}.",
        f"- Routing authority lexical candidates: {len(p['routing_authority_lexical_scan'])}.",
        f"- Mirror identity lexical candidates: {len(p['mirror_identity_lexical_scan'])}.",
        f"- Device identity lexical candidates: {len(p['device_identity_lexical_scan'])}.",
        f"- Stable-device carrier sites: {len(p['stable_device_carrier_sites'])}.",
        f"- Target identity lexical candidates: {len(p['target_identity_lexical_scan'])}.",
        f"- Host-config lexical candidates: {len(p['host_config_lexical_scan'])}.",
        f"- Semantic execution consumers: {len(p['execution_authority_consumers'])}.",
        f"- Document-only exemptions: {len(p['authored_document_exemptions'])}.",
        f"- ProcessBlock send expressions: {len(p['process_block_senders'])}.",
        f"- Event-ring write expressions: {len(p['event_ring_write_sites'])}.",
        f"- Replay protocol sites: {len(p['replay_protocol_sites'])}; "
        f"lifecycle lexical sites: {len(p['replay_lifecycle_lexical_scan'])}; "
        f"receiver-order sites: {len(p['replay_receiver_order_sites'])}.",
        f"- Offline coordinator sites: {len(p['offline_coordinator_sites'])}.",
        f"- Document restore/identity sites: "
        f"{len(p['document_restore_and_identity_sites'])}.",
        f"- State artifact identity sites: {len(p['state_artifact_identity_sites'])}.",
        f"- Mapping/output/readiness/producer gates: {len(p['mapping_and_output_gates'])}.",
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


def validate_decision_matrices(manifest: dict) -> None:
    routing = manifest["routing_matrix"]
    lanes = ["midi_in", "midi_out", "audio_in", "audio_out", "sidechain"]
    kinds = ["None", "Master", "Track", "ExternalInput"]
    refuse(routing.get("lanes") != lanes or routing.get("kinds") != kinds,
           "routing matrix axes changed")
    rows = routing.get("rows")
    refuse(not isinstance(rows, list) or len(rows) != 20, "routing matrix is not 5x4")
    observed = [(row.get("lane"), row.get("kind")) for row in rows]
    expected = [(lane, kind) for lane in lanes for kind in kinds]
    refuse(observed != expected or len(set(observed)) != 20,
           "routing matrix does not cover each lane/kind exactly once")
    refuse(sum(row.get("valid") is True for row in rows) != 14,
           "routing matrix valid-row count changed")
    for row in rows:
        refuse(set(row) != {"lane", "kind", "valid", "effect", "id_rule"},
               f"routing row shape changed: {row.get('lane')}/{row.get('kind')}")
        refuse(not isinstance(row["valid"], bool) or not row["effect"] or not row["id_rule"],
               f"routing row is undecidable: {row['lane']}/{row['kind']}")
    normalization = routing.get("normalization", {})
    refuse(normalization.get("complementary_pairs") !=
           ["midi_out->midi_in", "audio_out->audio_in"],
           "routing complementary pairs changed")
    refuse(normalization.get("track_edge_latency_blocks") != 1,
           "routing Track latency is not one block")
    refuse(normalization.get("fan_in_order") !=
           ["sourceTrackId", "sourceBus", "channel"],
           "routing fan-in order changed")
    for key in ("exact_duplicate", "source_output_cardinality", "input_none",
                "input_track", "input_external", "output_none", "cycle_policy",
                "pre_fader_rule", "aux_child_rule"):
        refuse(not normalization.get(key), f"routing normalization rule missing: {key}")

    artifacts = manifest["artifact_presence_matrix"]
    artifact_rows = artifacts.get("rows")
    refuse(not isinstance(artifact_rows, list) or len(artifact_rows) != 4,
           "artifact presence matrix is not 2x2")
    combinations = [(row.get("state_blob"), row.get("parameter_manifest"))
                    for row in artifact_rows]
    refuse(combinations != [(False, False), (True, False), (False, True), (True, True)],
           "artifact presence matrix combinations changed")
    for row in artifact_rows:
        refuse(set(row) != {"state_blob", "parameter_manifest", "load_outcome",
                            "retained_for_save", "module_entries"},
               f"artifact row shape changed: {row}")
        refuse(not row["load_outcome"] or not row["retained_for_save"] or
               not row["module_entries"], "artifact row is undecidable")
    for key in ("present_file_rules", "legacy_precedence", "save_rules", "package_rules"):
        refuse(not artifacts.get(key), f"artifact decision rule missing: {key}")
    inventory = artifacts.get("inventory_contract")
    inventory_keys = {
        "schema6_document_fields", "entry_shape", "entry_identity", "entry_order",
        "transient_provenance", "generation_id", "generation_path", "save_commit_order",
        "schema6_load", "legacy_import", "module_consumption", "orphan_policy",
    }
    refuse(not isinstance(inventory, dict) or set(inventory) != inventory_keys,
           "artifact inventory contract shape changed")
    refuse(inventory["schema6_document_fields"] !=
           ["artifact_generation", "artifact_entries"],
           "schema-6 artifact document fields changed")
    for key in inventory_keys - {"schema6_document_fields"}:
        refuse(not isinstance(inventory[key], str) or not inventory[key],
               f"artifact inventory rule missing: {key}")
    for token in ("ExplicitAbsent", "LegacyOldKey", "Schema6Generation", "LiveCapture"):
        refuse(token not in inventory["transient_provenance"],
               f"artifact provenance state missing: {token}")
    refuse("SHA-256" not in inventory["generation_id"] or
           "empty inventory" not in inventory["generation_id"],
           "artifact generation identity weakened")
    refuse("atomically replace the ProjectDocument" not in inventory["save_commit_order"] or
           "prior document/generation authoritative" not in inventory["save_commit_order"],
           "artifact save commit order weakened")
    refuse("directory iteration is removed" not in inventory["module_consumption"],
           "artifact module provenance weakened")


def validate_graph_and_semantics(manifest: dict) -> None:
    validate_decision_matrices(manifest)
    records = manifest["records"]
    tests = manifest["test_cases"]
    ids = [record.get("id") for record in records]
    test_ids = [case.get("id") for case in tests]
    refuse(len(ids) != len(set(ids)) or set(ids) != EXPECTED_RECORD_IDS,
           "record id population changed")
    refuse(set(manifest["changed_records"]) != EXPECTED_RECORD_IDS,
           "schema-v9 changed-record closure is incomplete")
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
        "R-TRANSACTIONAL-EVENT-BATCH",
        "D-PRODUCTION-RECEIVER", "R-PROJECT-TARGET-MIGRATION",
    }
    refuse(not required.issubset(closure), "authorization dependency closure is incomplete")

    auth = manifest["implementation_authorization"]
    refuse(auth["before_dual_pass"] is not False, "implementation authorized before dual PASS")
    refuse(auth["after_same_sha_semantic_and_evidence_pass"] is not True,
           "same-SHA dual PASS no longer authorizes declared scope")

    authority = by_id["R-HOST-PLAN-AUTHORITY"]["statement"]
    for token in (
        "session ExecutionSnapshot", "normalized one-block routing graph and reduce order",
        "one atomic snapshot publication", "HostConfig plugin vectors",
        "TrackStateSnapshot.chainDevices", "TrackStateSnapshot.routing", "routesToMaster",
        "project-global nextDeviceId", "undo/redo document", "revision exhaustion",
    ):
        refuse(token not in authority, f"session authority weakened: missing {token}")
    routing = by_id["R-ROUTING-AUTHORITY"]["statement"]
    for token in ("session ExecutionSnapshot revision", "machine-readable routing_matrix",
                  "all 20 rows", "None contributes no declaration",
                  "aux children are derived parent-owned output-bus projections",
                  "audio_out Master reaches master only",
                  "ExternalInput is valid only for midi_in, audio_in, and sidechain",
                  "one-sided Track declarations create an edge",
                  "exact input/output duplicates coalesce once",
                  "each source has at most one Track or Master sink",
                  "input None permits output-declared fan-in",
                  "input Track(source) constrains", "input ExternalInput conflicts",
                  "A.audioOut=B with B.audioIn=C",
                  "block N-1", "graph cycles are legal", "one-block-per-edge feedback",
                  "ascending {sourceTrackId, sourceBus, channel}",
                  "sidechain Track edge is additive", "one block per Track edge",
                  "without duplicating it to master", "routesToMaster atomic are removed",
                  "no old/new route combination", "order-dependent fan-in"):
        refuse(token not in routing, f"routing authority weakened: missing {token}")
    device_ids = by_id["R-DEVICE-ID-LIFETIME"]["statement"]
    for token in ("project-global", "[1, kStableDeviceIdMax]", "exactly 0x7FFF",
                  "validates before conversion", "never masks, wraps", "next_device_id",
                  "never reuses", "undo/redo", "0x8000", "globally duplicate",
                  "{trackId, oldDeviceId}->newDeviceId", "rewrites every device",
                  "LegacyArtifactKey{trackId, oldDeviceId}", "pluginStateFileName",
                  "pluginParamsFileName", "machine-readable artifact_presence_matrix",
                  "all four independent optional", "absence is successful",
                  "present unreadable/empty blob", "malformed/key-mismatched manifest",
                  "manifest's embedded ids are rewritten",
                  "transient provenance is ExplicitAbsent or Present",
                  "artifact_generation plus sorted artifact_entries",
                  "writes and verifies a fresh immutable generation",
                  "atomically replaces the document reference",
                  "resolve only that exact generation and inventory",
                  "never enumerate the state directory",
                  "absent old blob or manifest", "stale newly allocated canonical-looking file",
                  "never loaded, retained, or packaged", "Unreferenced generations",
                  "Exhaustion", "fails load"):
        refuse(token not in device_ids, f"device-id lifetime weakened: missing {token}")
    mirror = by_id["R-MIRROR-INSTANCE-IDENTITY"]["statement"]
    for token in ("{projectGlobalStableDeviceId, parameterUid16}", "all-target",
                  "one mirror entry per concrete", "distinct track plan"):
        refuse(token not in mirror, f"mirror instance identity weakened: missing {token}")
    ack = by_id["R-CORRELATED-REPLAY-ACK"]["statement"]
    for token in ("ReplayCompletePayload", "max(lastIssuedGate, liveAck)+1", "UINT64_MAX",
                  "strictly below"):
        refuse(token not in ack, f"correlated acknowledgement weakened: missing {token}")
    mirror_epoch = by_id["R-MIRROR-EPOCH"]["statement"]
    for token in ("Unprimed", "AwaitingAck", "Complete", "FailedPermanent",
                  "AwaitingAck never re-primes", "no separate primed flag"):
        refuse(token not in mirror_epoch, f"mirror state machine weakened: missing {token}")
    transactional = by_id["R-TRANSACTIONAL-EVENT-BATCH"]["statement"]
    for token in ("All six frozen host-event ring writers", "SessionBlockPlan",
                  "sessionBlockTicket", "Phase 1", "canonical stable host order",
                  "preflights every ring", "before reserving any", "no participant reserves",
                  "audio-input recipe", "OrdinaryFailedPermanent", "no automatic retry occurs",
                  "realtime transport stops", "offline sets renderFailed",
                  "Phase 2", "partial/full control-frame failure", "quarantined",
                  "realtime playback stops", "offline rendering fails",
                  "matching output resolves the immutable master-input recipe",
                  "No permanently oversized or partially successful session block"):
        refuse(token not in transactional, f"transactional batch weakened: missing {token}")
    capacity = by_id["R-ATOMIC-PRIMER-CAPACITY"]["statement"]
    for token in ("ringWriteBatch", "one CAS", "ready=0", "first reserved ready flag last",
                  "phase 1", "N>=C ordinary entries as OrdinaryFailedPermanent",
                  "C-2", "N>=C-1", "Every preflight and batch result is consumed"):
        refuse(token not in capacity, f"capacity boundary weakened: missing {token}")
    master = by_id["R-MASTER-CORRELATION"]["statement"]
    for token in ("hostGeneration", "dispatchTicket", "sessionBlockTicket", "mirrorEpoch",
                  "mirrorStage", "blockId", "same ordinary SessionBlockPlan",
                  "Boolean hostReady/masterFxActive cannot authorize"):
        refuse(token not in master, f"master correlation weakened: missing {token}")
    offline = by_id["R-OFFLINE-PRIMER"]["statement"]
    for token in ("ControlPreroll", "transport is stopped", "SessionBlockPlan",
                  "commits no ring until all can reserve", "renderFailed"):
        refuse(token not in offline, f"offline phase closure weakened: missing {token}")
    project = by_id["R-PROJECT-TARGET-MIGRATION"]["statement"]
    for token in ("schema advances 4 to 6", "top-level next_device_id",
                  "artifact_generation", "artifact_entries", "tagged automation target",
                  "DisabledLegacyCompact", "legacy_target_plugin_index", "disabled_reason",
                  "unchanged lane events", "snapshot compilation and dispatch exclude",
                  "schema 1-5", "never mapped through the current machine", "Schema 6 rejects",
                  "round-trips every target tag"):
        refuse(token not in project, f"project migration weakened: missing {token}")
    receiver = by_id["D-PRODUCTION-RECEIVER"]["statement"]
    for token in ("used by handleProcessBlock", "eventPriority", "Param precedes ReplayComplete",
                  "resets replayAckGate", "actual application", "runControlLoop"):
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
        "ack_reset": "replayAckSampleTime.store",
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

    writer_classes = {
        "ordinary_host_event_writer": 4,
        "mirror_host_event_writer": 2,
        "ui_diff_non_host_writer": 1,
        "host_key_non_dispatch_writer": 1,
    }
    observed_classes: dict[str, int] = {}
    for entry in manifest["populations"]["event_ring_write_sites"]:
        line = frozen_text(entry["path"]).splitlines()[entry["line"] - 1]
        refuse("daw::ringWrite" not in line, f"event writer locator is not a writer: {entry}")
        classification = entry.get("classification")
        observed_classes[classification] = observed_classes.get(classification, 0) + 1
    refuse(observed_classes != writer_classes, "event writer classifications changed")

    for name, classification in (
        ("mapping_and_output_gates", "readiness_output_dispatch_dataflow_candidate"),
        ("state_artifact_identity_sites", "plugin_artifact_key_directory_or_package_boundary"),
    ):
        refuse(any(entry.get("classification") != classification
                   for entry in manifest["populations"][name]),
               f"derived population classification changed: {name}")


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
        "stable_device_carrier_sites": derive_named_paths_scan(
            frozen["commit"], CARRIER_SCAN_RE, CARRIER_SCAN_PATHS,
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
        "replay_lifecycle_lexical_scan": derive_line_scan(
            frozen["commit"], REPLAY_LIFECYCLE_RE, skip_line_comments=True,
        ),
        "event_ring_write_sites": derive_line_scan(
            frozen["commit"], EVENT_RING_WRITE_RE,
        ),
        "document_restore_and_identity_sites": derive_named_paths_scan(
            frozen["commit"], DOCUMENT_IDENTITY_RE, DOCUMENT_IDENTITY_PATHS,
            skip_line_comments=True,
        ),
        "mapping_and_output_gates": derive_mapping_output_gates(frozen["commit"]),
        "state_artifact_identity_sites": derive_state_artifact_sites(frozen["commit"]),
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
    refuse(manifest["schema"] != "ae-p1.2-g2b-item18-packet/9", "schema is not v9")
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
    # Scanner controls are synthetic and count in the same exact total as manifest mutations, so
    # deleting a fixture cannot preserve a misleading N/N report.
    scanner_fixtures = {
        "renamed project chain mutation": (MUTATION_SCAN_RE, "lane.chain.devices.push_back(device);"),
        "chain alias declaration": (
            MUTATION_SCAN_RE, "auto& devs = document.tracks.front().chain.devices;"
        ),
        "renamed pointer patcher sink": (
            MUTATION_SCAN_RE, "chosen->patcher = patcherGraphState.graph;"
        ),
        "renamed value patcher-node sink": (
            MUTATION_SCAN_RE, "device.patcherNodeId = pooledNode;"
        ),
        "routing alias": (ROUTING_SCAN_RE, "runtime->routesToMaster.store(true);"),
        "mirror key": (MIRROR_SCAN_RE, "runtime.paramMirror[key] = value;"),
        "device watermark input": (DEVICE_ID_SCAN_RE, "uint32_t nextDeviceId = 1;"),
        "durable target identity": (TARGET_ID_SCAN_RE, "clip.targetPluginIndex();"),
        "host config consumer": (HOST_CONFIG_SCAN_RE, "runtime.config.pluginPaths = paths;"),
        "stable carrier producer": (
            CARRIER_SCAN_RE, "device_id: opaque(args, \"device\") as u16,"
        ),
        "stable carrier consumer": (
            CARRIER_SCAN_RE, "daw::UiSamplerSlotNameHeader header{};"
        ),
        "replay stage lifecycle": (REPLAY_LIFECYCLE_RE, "runtime.mirrorPending.store(true);"),
        "event ring writer": (EVENT_RING_WRITE_RE, "daw::ringWrite(events, entry);"),
        "document restore renamed argument": (
            DOCUMENT_IDENTITY_RE, "deps.applyDocument(candidate);"
        ),
        "renamed undo owner": (DOCUMENT_IDENTITY_RE, "timeline.undo();"),
        "renamed document visitor": (
            DOCUMENT_IDENTITY_RE, 'walker.field("device_id", &Device::id, 0u);'
        ),
        "renamed readiness receiver": (
            MAPPING_GATE_BASE_RE,
            "if (!candidate->hostReady.load(std::memory_order_acquire)) {}",
        ),
        "state artifact key": (
            STATE_ARTIFACT_HELPER_RE, "pluginStateFileName(track, id);"
        ),
        "renamed package entry": (
            STATE_ARTIFACT_PACKAGE_RE, "archive.name = stem + leaf;"
        ),
        "receiver priority": (REPLAY_ORDER_RE, "return EventType::Param;"),
        "offline call": (OFFLINE_ENTRYPOINT_RE, "daw::engine::runOfflinePump(deps);"),
    }
    for name, (pattern, text) in scanner_fixtures.items():
        refuse(pattern.search(text) is None, f"scanner fixture missed: {name}")

    renamed_gate_source = """auto& enabled = deps.masterFxActive;
if (!enabled.load(std::memory_order_acquire)) {}
bool primer = false;
if (node->mirrorPending.load(std::memory_order_acquire) &&
    !node->mirrorPrimed.load(std::memory_order_acquire)) {
  primer = true;
}
if (!primer && playing) {}
uint32_t witness = node->completedBlockId.load(std::memory_order_acquire);
if (witness > 0) {}
if (!candidate->hostReady.load(std::memory_order_acquire)) {}
EngineAudioCallback::TrackInfo projection;
candidate->controller.sendProcessBlock(block);
"""
    expected_renamed_gates = tuple(("apps/renamed.cpp", line) for line in
                                   (1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13))
    refuse(derive_mapping_output_gates_from_sources(
        {"apps/renamed.cpp": renamed_gate_source}
    ) != expected_renamed_gates, "renamed readiness/output dataflow fixture missed")

    renamed_package_source = """pluginStateFileName(track_id, device_id);
for (const auto& item : std::filesystem::directory_iterator(root)) {}
names.push_back(item.path().filename().string());
ZipEntry archive;
archive.name = stem + leaf;
"""
    expected_package = tuple(("apps/project_file.cpp", line) for line in (1, 2, 3, 5))
    refuse(derive_state_artifact_sites_from_sources({
        "apps/project_file.cpp": renamed_package_source,
        "apps/other.cpp":
            "for (auto& x : std::filesystem::directory_iterator(root)) {}\n",
    }) != expected_package, "renamed package-boundary fixture missed")
    structural_scanner_controls = 2

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
    candidate = copy.deepcopy(manifest)
    candidate["routing_matrix"]["rows"].pop()
    add("routing matrix row deletion", candidate)
    candidate = copy.deepcopy(manifest)
    candidate["routing_matrix"]["rows"][0]["valid"] = False
    add("routing matrix result substitution", candidate)
    candidate = copy.deepcopy(manifest)
    candidate["artifact_presence_matrix"]["rows"].pop()
    add("artifact matrix row deletion", candidate)
    candidate = copy.deepcopy(manifest)
    candidate["artifact_presence_matrix"]["package_rules"] = ""
    add("artifact package rule deletion", candidate)
    candidate = copy.deepcopy(manifest)
    candidate["artifact_presence_matrix"]["inventory_contract"].pop("module_consumption")
    add("artifact inventory provenance deletion", candidate)

    for population in sorted(manifest["populations"]):
        candidate = copy.deepcopy(manifest)
        candidate["populations"][population].pop()
        add(f"population deletion: {population}", candidate)

    for record_id in (
        "R-HOST-PLAN-AUTHORITY", "R-ROUTING-AUTHORITY", "R-DEVICE-ID-LIFETIME",
        "R-STABLE-DEVICE-TARGETS", "R-MIRROR-INSTANCE-IDENTITY", "R-DISPATCH-TICKET",
        "R-MIRROR-EPOCH", "R-CORRELATED-REPLAY-ACK", "R-ATOMIC-PRIMER-CAPACITY",
        "R-TRANSACTIONAL-EVENT-BATCH", "R-PASS4-REPLACEMENT",
        "R-MASTER-CORRELATION", "R-OFFLINE-PRIMER",
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
        "T-PROJECT-TARGET-MIGRATION", "T-ORDINARY-BATCH-ATOMIC",
        "T-NO-DRAIN-LEAK", "T-PRODUCER-GATES", "T-GLOBAL-DEVICE-IDENTITY",
        "T-SESSION-BLOCK-ATOMIC", "T-LEGACY-DISABLED-ROUNDTRIP",
        "T-ALL-EVENT-WRITERS", "T-STATE-ARTIFACT-MIGRATION",
        "T-ROUTING-BLOCK-DETERMINISM", "T-ORDINARY-CAPACITY-PERMANENT",
        "T-ROUTING-MATRIX", "T-ARTIFACT-PRESENCE-MATRIX",
        "T-ARTIFACT-PROVENANCE",
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
    controls = len(scanner_fixtures) + structural_scanner_controls + len(cases)
    refused_controls = len(scanner_fixtures) + structural_scanner_controls + refused
    refuse(controls != EXPECTED_MUTATION_CONTROLS,
           f"mutation-control population changed: {controls}")
    refuse(refused_controls != EXPECTED_MUTATION_CONTROLS,
           f"mutation controls did not all refuse: "
           f"{refused_controls}/{EXPECTED_MUTATION_CONTROLS}")
    return refused_controls


def main() -> int:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    if "--render" in sys.argv:
        print(render(manifest), end="")
        return 0
    validate(manifest)
    controls = self_test(manifest)
    p = manifest["populations"]
    print("AE-P1.2 G2-B item 18 schema-v9 packet: PASS")
    print(f"  records: {len(manifest['records'])}")
    print(f"  tests: {len(manifest['test_cases'])}")
    print(f"  routing decision rows: {len(manifest['routing_matrix']['rows'])}")
    print(f"  artifact presence rows: {len(manifest['artifact_presence_matrix']['rows'])}")
    print(f"  governed files: {len(manifest['governed_files'])}")
    print(f"  readiness publishers: {len(p['host_ready_true_sites'])}")
    print(f"  snapshot publications: {len(p['track_snapshot_publications'])}")
    print(f"  mutation candidates: {len(p['chain_mutation_scan'])}")
    print(f"  chain/path authority candidates: {len(p['execution_authority_lexical_scan'])}")
    print(f"  routing candidates: {len(p['routing_authority_lexical_scan'])}")
    print(f"  mirror identity candidates: {len(p['mirror_identity_lexical_scan'])}")
    print(f"  device identity candidates: {len(p['device_identity_lexical_scan'])}")
    print(f"  stable-device carrier sites: {len(p['stable_device_carrier_sites'])}")
    print(f"  target identity candidates: {len(p['target_identity_lexical_scan'])}")
    print(f"  host-config candidates: {len(p['host_config_lexical_scan'])}")
    print(f"  semantic consumers: {len(p['execution_authority_consumers'])}")
    print(f"  ProcessBlock senders: {len(p['process_block_senders'])}")
    print(f"  event-ring writers: {len(p['event_ring_write_sites'])}")
    print(f"  replay lifecycle sites: {len(p['replay_lifecycle_lexical_scan'])}")
    print(f"  document restore/identity sites: {len(p['document_restore_and_identity_sites'])}")
    print(f"  state artifact identity sites: {len(p['state_artifact_identity_sites'])}")
    print(f"  mapping/output/readiness/producer gates: {len(p['mapping_and_output_gates'])}")
    print(f"  mutation controls: {controls}/{EXPECTED_MUTATION_CONTROLS} refused")
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
