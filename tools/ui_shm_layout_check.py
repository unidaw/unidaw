#!/usr/bin/env python3
"""Static AE-P1.3 ratchet for the complete UI-SHM region population."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "apps/engine_ui_shm.cpp"
LAYOUT = ROOT / "ui/daw-bridge/src/layout.rs"
CONTROL = ROOT / "ui/daw-bridge/src/control.rs"


def fail(message: str) -> None:
    print(f"ui_shm_layout_check: FAIL: {message}")
    raise SystemExit(1)


def unique(values: list[str], label: str) -> None:
    duplicates = sorted({value for value in values if values.count(value) > 1})
    if duplicates:
        fail(f"{label} contains duplicates: {duplicates}")


def camel_to_snake(value: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", value).lower()


engine = ENGINE.read_text()
layout = LAYOUT.read_text()
control = CONTROL.read_text()

producer_camel = [
    f"{stem}Offset"
    for stem in re.findall(r"header\.([A-Za-z0-9]+)Offset\s*=\s*offset\s*;", engine)
]
producer = [
    f"{camel_to_snake(field.removesuffix('Offset'))}_offset" for field in producer_camel
]

header_start = layout.find("pub struct ShmHeader {")
header_end = layout.find("\n}", header_start)
if header_start < 0 or header_end < 0:
    fail("cannot isolate Rust ShmHeader")
rust_fields = re.findall(
    r"^\s*pub ([a-z0-9_]+_offset): u64,\s*$",
    layout[header_start:header_end],
    re.M,
)

descriptor_start = control.find("struct UiShmDescriptor {")
descriptor_end = control.find("\n}", descriptor_start)
if descriptor_start < 0 or descriptor_end < 0:
    fail("cannot isolate UiShmDescriptor")
descriptor_fields = re.findall(
    r"^\s*([a-z0-9_]+): u(?:32|64),\s*$",
    control[descriptor_start:descriptor_end],
    re.M,
)

read_start = control.find("unsafe fn read(header: *const ShmHeader)", descriptor_end)
read_end = control.find("\n    }\n}", read_start)
if read_start < 0 or read_end < 0:
    fail("cannot isolate UiShmDescriptor::read")
read_pairs = re.findall(
    r"^\s*([a-z0-9_]+): field!\(([a-z0-9_]+)\),\s*$",
    control[read_start:read_end],
    re.M,
)
read_destinations = [destination for destination, _ in read_pairs]
if descriptor_fields != read_destinations:
    fail(
        "UiShmDescriptor::read does not initialize every descriptor field in declaration order: "
        f"descriptor={descriptor_fields}, read={read_destinations}"
    )
miswired = [
    (destination, source)
    for destination, source in read_pairs
    if destination != source
]
if miswired:
    fail(f"UiShmDescriptor::read has field-to-field miswiring: {miswired}")

enum_match = re.search(r"enum UiRegionId \{(?P<body>.*?)^\}", control, re.M | re.S)
if not enum_match:
    fail("cannot isolate UiRegionId")
enum_variants = re.findall(r"^\s*([A-Z][A-Za-z0-9]+),\s*$", enum_match.group("body"), re.M)
if not enum_variants or enum_variants[-1] != "Count":
    fail("UiRegionId must end in Count")
validator_ids = [camel_to_snake(variant) for variant in enum_variants[:-1]]

validator_calls = re.findall(
    r"cache_ui_shm_region!\s*\(\s*([A-Z][A-Za-z0-9]+)\s*,\s*\"([a-z0-9_]+)\"",
    control,
    re.S,
)
validator_call_variants = [variant for variant, _ in validator_calls]
validator_call_ids = [name for _, name in validator_calls]

for values, label in [
    (producer, "producer offsets"),
    (rust_fields, "Rust offsets"),
    (validator_ids, "validator enum"),
    (validator_call_ids, "validator table"),
]:
    unique(values, label)

expected_count = 25
counts = {
    "producer": len(producer),
    "rust": len(rust_fields),
    "validator_enum": len(validator_ids),
    "validator_table": len(validator_call_ids),
}
if any(count != expected_count for count in counts.values()):
    fail(f"region counts are {counts}, expected every population to be {expected_count}")
if set(producer) != set(rust_fields):
    fail(
        "producer/Rust region identities differ: "
        f"producer_only={sorted(set(producer) - set(rust_fields))}, "
        f"rust_only={sorted(set(rust_fields) - set(producer))}"
    )
if validator_ids != validator_call_ids:
    fail(f"validator enum/table order drift: enum={validator_ids}, table={validator_call_ids}")
if producer != [f"{name}_offset" for name in validator_call_ids]:
    fail("validator table does not follow the producer's placement order")
if enum_variants[:-1] != validator_call_variants:
    fail("validator calls do not use the corresponding UiRegionId variant")
if "ui_command_outcome" not in validator_call_ids:
    fail("the v41 command-outcome region is absent from the validator")
if set(rust_fields) != {
    field for field in descriptor_fields if field.endswith("_offset")
}:
    fail("UiShmDescriptor offset population differs from the Rust ShmHeader population")

asserted_offsets = re.findall(r"offset_of!\(ShmHeader, ([a-z0-9_]+_offset)\)", layout)
unique(asserted_offsets, "Rust ShmHeader offset assertions")
if set(asserted_offsets) != set(rust_fields):
    fail(
        "Rust offset assertions are incomplete: "
        f"missing={sorted(set(rust_fields) - set(asserted_offsets))}, "
        f"extra={sorted(set(asserted_offsets) - set(rust_fields))}"
    )

geometry_fields = [
    field
    for field in descriptor_fields
    if field.endswith("_offset") or field.endswith("_bytes")
]
geometry_pattern = re.compile(
    r"\.\s*(" + "|".join(map(re.escape, geometry_fields)) + r")\b"
)

# Production geometry field accesses are legal only through the descriptor copied once during
# attach. This is deliberately receiver-agnostic: `(*self.header).field`, `alias.field`, and a
# helper's `header.field` all contain the same field selector and all fail. Tests may mutate their
# synthetic descriptor/header after this boundary; they are not product accessors.
tests_at = control.find("#[cfg(test)]\nmod layout_non_overlap_tests")
if tests_at < 0:
    fail("cannot isolate layout_non_overlap_tests")
for match in geometry_pattern.finditer(control[:tests_at]):
    receiver = control[max(0, match.start() - 32) : match.start()]
    if not re.search(r"\bdescriptor\s*$", receiver):
        line = control.count("\n", 0, match.start()) + 1
        fail(
            f"control.rs:{line} reads geometry outside the one-shot UiShmDescriptor: "
            f"{match.group(1)}"
        )

engine_impl_start = control.find("impl EngineHandle {")
engine_impl_end = control.find("\n}\n\nfn poll_command_outcome_region", engine_impl_start)
if engine_impl_start < 0 or engine_impl_end < 0:
    fail("cannot isolate EngineHandle implementation")
engine_impl_geometry = geometry_pattern.findall(
    control[engine_impl_start:engine_impl_end]
)
if engine_impl_geometry:
    fail(
        "EngineHandle accessors mention mutable geometry fields instead of the validated cache: "
        f"{sorted(set(engine_impl_geometry))}"
    )

attach_start = control.find("fn attach_inner(")
attach_end = control.find("\n    /// A pointer to a `T`", attach_start)
if attach_start < 0 or attach_end < 0:
    fail("cannot isolate EngineHandle::attach_inner")
attach = control[attach_start:attach_end]
descriptor_at = attach.find("UiShmDescriptor::read")
validate_at = attach.find("validate_ui_shm_layout")
view_at = attach.find(".event_view")
publish_at = attach.find("Ok(Self")
if not (0 <= descriptor_at < validate_at < view_at < publish_at):
    fail(
        "attach order must be descriptor read -> complete validation -> typed ring view -> EngineHandle publication"
    )

validator_start = control.find("fn validate_ui_shm_layout(")
validator_end = control.find("\n/// Does a `size`-byte region", validator_start)
if validator_start < 0 or validator_end < 0:
    fail("cannot isolate validate_ui_shm_layout")
validator = control[validator_start:validator_end]
if 'name: "mapping_header"' not in validator:
    fail("the reserved mapping-header span is absent")
if "spans.retain(|span| span.start != span.end)" not in validator:
    fail("zero-length half-open spans are not explicitly excluded from overlap comparison")
if "current.start < previous.end" not in validator:
    fail("the half-open overlap predicate is absent")

print(
    "ui_shm_layout_check: PASS: "
    "25 producer offsets = 25 Rust offsets = 25 validator entries; "
    "26th reserved header span; exact descriptor wiring; cached geometry only"
)
