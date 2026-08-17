#!/usr/bin/env python3
"""Validate and render the AE-P1.3 non-overlap implementation packet."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = ROOT / "docs/architecture/tasks/AE-P1.3-nonoverlap-manifest.json"
DOC_PATH = ROOT / "docs/architecture/tasks/AE-P1.3-nonoverlap.md"
ENGINE_PATH = ROOT / "apps/engine_ui_shm.cpp"
LAYOUT_PATH = ROOT / "ui/daw-bridge/src/layout.rs"
CONTROL_PATH = ROOT / "ui/daw-bridge/src/control.rs"


def fail(message: str) -> "None":
    raise SystemExit(f"AE-P1.3 packet check FAILED: {message}")


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, text=True, capture_output=True, check=False
    )
    if result.returncode != 0:
        fail(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout.strip()


def camel_to_snake(value: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", value).lower()


def derive_producer_fields(text: str) -> list[str]:
    return [
        f"{stem}Offset"
        for stem in re.findall(r"header\.([A-Za-z0-9]+)Offset\s*=\s*offset\s*;", text)
    ]


def derive_rust_fields(text: str) -> list[str]:
    marker = "pub struct ShmHeader {"
    start = text.find(marker)
    if start < 0:
        fail("Rust ShmHeader start not found")
    end = text.find("\n}", start)
    if end < 0:
        fail("Rust ShmHeader end not found")
    return re.findall(r"^\s*pub ([a-z0-9_]+_offset): u64,\s*$", text[start:end], re.M)


def require_unique(values: list[str], label: str) -> None:
    duplicates = sorted({value for value in values if values.count(value) > 1})
    if duplicates:
        fail(f"{label} contains duplicates: {duplicates}")


def render(manifest: dict) -> str:
    records = manifest["records"]
    by_kind: dict[str, list[dict]] = {}
    for record in records:
        by_kind.setdefault(record["kind"], []).append(record)

    frozen = manifest["frozen_product"]
    lines = [
        "# AE-P1.3 — whole-layout non-overlap residue",
        "",
        "> Generated from `AE-P1.3-nonoverlap-manifest.json`; do not edit by hand.",
        "",
        f"Status: `{manifest['status']}`. Owner: `{manifest['owner']}`.",
        f"Frozen product: `{frozen['commit']}` (tree `{frozen['tree']}`).",
        "",
        "## Scope",
        "",
        manifest["scope"],
        "",
        "## Gate",
        "",
    ]
    for record in by_kind["gate"]:
        lines.append(f"- `{record['id']}` [{record['status']}]: {record['statement']}")
    lines += ["", "## Region population", "", "| Region | Producer field | Rust field | Kind | Size rule |", "|---|---|---|---|---|"]
    for region in manifest["regions"]:
        lines.append(
            f"| `{region['id']}` | `{region['producer_field']}` | "
            f"`{region['rust_field']}` | `{region['kind']}` | {region['size_rule']} |"
        )

    count_records = by_kind["count"]
    lines += ["", "## Derived counts", ""]
    for record in count_records:
        lines.append(f"- `{record['id']}` = {record['value']}: {record['statement']}")

    for heading, kind in [
        ("Rulings", "ruling"),
        ("Implementation decisions", "implementation_decision"),
        ("Controls", "control"),
    ]:
        lines += ["", f"## {heading}", ""]
        for record in by_kind[kind]:
            deps = ", ".join(f"`{dep}`" for dep in record["dependencies"]) or "none"
            lines.append(
                f"- `{record['id']}` [{record['status']}], dependencies {deps}: "
                f"{record['statement']}"
            )

    lines += ["", "## Non-goals", ""]
    lines.extend(f"- {item}" for item in manifest["non_goals"])
    lines += [
        "",
        "## Review requirement",
        "",
        "Implementation is authorized only after independent semantic and evidence reviewers both return PASS for the same immutable packet SHA and frozen product base.",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    manifest = json.loads(MANIFEST_PATH.read_text())
    required_top = {
        "schema",
        "ticket",
        "status",
        "owner",
        "frozen_product",
        "scope",
        "non_goals",
        "regions",
        "records",
    }
    if set(manifest) != required_top:
        fail(f"top-level keys are {sorted(manifest)}, expected {sorted(required_top)}")
    if manifest["schema"] != "ae-p1.3-nonoverlap-packet/1":
        fail("unknown schema")
    if manifest["status"] != "REVIEW_CANDIDATE":
        fail("packet must be an immutable REVIEW_CANDIDATE")

    frozen = manifest["frozen_product"]
    if git("rev-parse", f"{frozen['commit']}^{{tree}}") != frozen["tree"]:
        fail("frozen product tree does not match its commit")
    governed = [
        "apps/engine_ui_shm.cpp",
        "ui/daw-bridge/src/layout.rs",
        "ui/daw-bridge/src/control.rs",
    ]
    if git("diff", "--name-only", frozen["commit"], "--", *governed):
        fail("governed product paths differ from the frozen product")

    regions = manifest["regions"]
    region_ids = [region["id"] for region in regions]
    producer_fields = [region["producer_field"] for region in regions]
    rust_fields = [region["rust_field"] for region in regions]
    for values, label in [
        (region_ids, "region ids"),
        (producer_fields, "producer fields"),
        (rust_fields, "Rust fields"),
    ]:
        require_unique(values, label)
    for region in regions:
        producer_stem = region["producer_field"].removesuffix("Offset")
        expected_rust = f"{camel_to_snake(producer_stem)}_offset"
        if region["rust_field"] != expected_rust:
            fail(f"{region['id']} maps {region['producer_field']} to {region['rust_field']}, expected {expected_rust}")

    derived_producer = derive_producer_fields(ENGINE_PATH.read_text())
    derived_rust = derive_rust_fields(LAYOUT_PATH.read_text())
    require_unique(derived_producer, "derived producer fields")
    require_unique(derived_rust, "derived Rust fields")
    if producer_fields != derived_producer:
        fail(f"producer population drift: manifest={producer_fields}, derived={derived_producer}")
    if set(rust_fields) != set(derived_rust) or len(rust_fields) != len(derived_rust):
        fail(f"Rust population drift: manifest={rust_fields}, derived={derived_rust}")

    records = manifest["records"]
    record_ids = [record.get("id", "") for record in records]
    require_unique(record_ids, "record ids")
    required_record = {
        "id",
        "kind",
        "owner",
        "status",
        "dependencies",
        "source_span",
        "control",
        "statement",
    }
    allowed_kinds = {
        "gate",
        "dependency",
        "population",
        "count",
        "ruling",
        "implementation_decision",
        "control",
    }
    for record in records:
        missing = required_record - set(record)
        if missing:
            fail(f"record {record.get('id')} missing {sorted(missing)}")
        if record["kind"] not in allowed_kinds:
            fail(f"record {record['id']} has unknown kind {record['kind']}")
        if record["owner"] != manifest["owner"]:
            fail(f"record {record['id']} has a different owner")
        for dependency in record["dependencies"]:
            if dependency not in record_ids:
                fail(f"record {record['id']} depends on unknown {dependency}")
        if record["control"] not in record_ids:
            fail(f"record {record['id']} names unknown control {record['control']}")

    expected_kinds = {
        "gate",
        "dependency",
        "population",
        "count",
        "ruling",
        "implementation_decision",
        "control",
    }
    present_kinds = {record["kind"] for record in records}
    if present_kinds != expected_kinds:
        fail(f"record kinds are {sorted(present_kinds)}, expected {sorted(expected_kinds)}")

    counts = {record["id"]: record["value"] for record in records if record["kind"] == "count"}
    expected_counts = {
        "C-OFFSET-REGIONS": len(regions),
        "C-COMPARED-SPANS": len(regions) + 1,
    }
    if counts != expected_counts:
        fail(f"count records are {counts}, expected {expected_counts}")

    control_records = {record["id"] for record in records if record["kind"] == "control"}
    for record in records:
        if record["control"] not in control_records:
            fail(f"record {record['id']} control {record['control']} is not a control record")

    # The frozen attach still lacks the residue, which prevents a packet from silently reviewing
    # an implementation that is already changing under it.
    control_text = CONTROL_PATH.read_text()
    if "validate_ui_shm_layout" in control_text or "layout_non_overlap" in control_text:
        fail("frozen product already contains the planned validator/test namespace")

    expected_doc = render(manifest)
    if "--render" in sys.argv:
        sys.stdout.write(expected_doc)
        return
    if not DOC_PATH.exists():
        fail(f"generated prose is absent; run {Path(__file__).name} --render")
    actual_doc = DOC_PATH.read_text()
    if actual_doc != expected_doc:
        fail("generated prose differs from the manifest renderer")
    print(
        "AE-P1.3 packet PASS: "
        f"{len(regions)} offset regions, {len(regions) + 1} compared spans, "
        f"{len(records)} manifest records"
    )


if __name__ == "__main__":
    main()
