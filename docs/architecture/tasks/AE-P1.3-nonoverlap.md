# AE-P1.3 — whole-layout non-overlap residue

> Generated from `AE-P1.3-nonoverlap-manifest.json`; do not edit by hand.

Status: `REVIEW_CANDIDATE`. Owner: `backend`.
Frozen product: `0d943c262393101d88e98cb94aff14783b9aa2be` (tree `27eaec16a10cae793f48cb074c07467130e471a3`).
Program source: `22eba6fe82cead0e1442fb3db6d0165edb7f5d91` (tree `642a1a2c43deb5e8f1ce5360563fc5bdec88582d`).
Successor to packet `6e65b838b590e9d1318d8267776144381f13613e` / manifest `c043367445422d9adbacc4d07e4c3b2f006edafca55b5ed76b6aa9d52b832f9a`.
Reopening reason: Both independent reviews found unresolvable source locators; semantic review also found a disconnected gate graph, and evidence review found incomplete size/ring negative controls.

## Scope

Complete AE-P1.3's UI shared-memory residue by refusing any non-empty typed region that aliases the mapping header or another non-empty region before EngineHandle exposes a typed view.

## Gate

- `G-NONOVERLAP` [READY_FOR_REVIEW]: No typed UI-SHM view is exposed until the complete descriptor is bounds-, alignment-, size-, and pairwise-non-overlap-valid.

## Region population

| Region | Producer field | Rust field | Kind | Size rule |
|---|---|---|---|---|
| `audio_in` | `audioInOffset` | `audio_in_offset` | `audio` | checked(num_channels_in * num_blocks * channel_stride_bytes) |
| `audio_out` | `audioOutOffset` | `audio_out_offset` | `audio` | checked(num_channels_out * num_blocks * channel_stride_bytes) |
| `ring_std` | `ringStdOffset` | `ring_std_offset` | `inactive_ring` | aligned RingHeader; capacity == 0 and entry_size == 0 |
| `ring_ctrl` | `ringCtrlOffset` | `ring_ctrl_offset` | `inactive_ring` | aligned RingHeader; capacity == 0 and entry_size == 0 |
| `ring_ui` | `ringUiOffset` | `ring_ui_offset` | `event_ring` | checked aligned RingHeader + capacity * size_of::<EventEntry>() |
| `ring_ui_out` | `ringUiOutOffset` | `ring_ui_out_offset` | `event_ring` | checked aligned RingHeader + capacity * size_of::<EventEntry>() |
| `ring_ui_edit` | `ringUiEditOffset` | `ring_ui_edit_offset` | `edit_ring` | capacity == K_UI_EDIT_BATCH_CAPACITY; entry_size == size_of::<UiEditBatchEntry>(); checked aligned RingHeader + capacity * entry_size |
| `mailbox` | `mailboxOffset` | `mailbox_offset` | `fixed` | size_of::<BlockMailbox>() |
| `ui_clip` | `uiClipOffset` | `ui_clip_offset` | `declared_exact` | ui_clip_bytes == size_of::<UiClipWindowSnapshot>() |
| `ui_harmony` | `uiHarmonyOffset` | `ui_harmony_offset` | `declared_exact` | ui_harmony_bytes == size_of::<UiHarmonySnapshot>() |
| `ui_clip_all` | `uiClipAllOffset` | `ui_clip_all_offset` | `declared_exact` | ui_clip_all_bytes == checked(K_UI_MAX_TRACKS * size_of::<UiClipWindowSnapshot>()) |
| `ring_ui_agent` | `ringUiAgentOffset` | `ring_ui_agent_offset` | `event_ring` | checked aligned RingHeader + capacity * size_of::<EventEntry>() |
| `ui_clip_extent` | `uiClipExtentOffset` | `ui_clip_extent_offset` | `fixed` | size_of::<UiClipExtentRegion>() |
| `ui_patcher` | `uiPatcherOffset` | `ui_patcher_offset` | `fixed` | size_of::<UiPatcherRegion>() |
| `ui_arrange` | `uiArrangeOffset` | `ui_arrange_offset` | `declared_exact` | ui_arrange_bytes == size_of::<UiArrangeSummaryRegion>() |
| `ui_automation` | `uiAutomationOffset` | `ui_automation_offset` | `declared_exact` | ui_automation_bytes == size_of::<UiAutomationLaneRegion>() |
| `ui_automation_slot` | `uiAutomationSlotOffset` | `ui_automation_slot_offset` | `declared_exact` | ui_automation_slot_bytes == size_of::<UiAutomationSlotRegion>() |
| `ui_device_meter` | `uiDeviceMeterOffset` | `ui_device_meter_offset` | `fixed` | size_of::<UiDeviceMeterRegion>() |
| `ui_scales` | `uiScalesOffset` | `ui_scales_offset` | `fixed` | size_of::<UiScaleRegion>() |
| `ui_device_params` | `uiDeviceParamsOffset` | `ui_device_params_offset` | `fixed` | size_of::<UiDeviceParamsRegion>() |
| `ui_audio_source` | `uiAudioSourceOffset` | `ui_audio_source_offset` | `fixed` | size_of::<UiAudioSourceRegion>() |
| `ui_waveform` | `uiWaveformOffset` | `ui_waveform_offset` | `fixed` | size_of::<UiWaveformRegion>() |
| `ui_sampler_kit` | `uiSamplerKitOffset` | `ui_sampler_kit_offset` | `declared_exact` | ui_sampler_kit_bytes == size_of::<UiSamplerKitRegion>() |
| `ui_sampler_envelope` | `uiSamplerEnvelopeOffset` | `ui_sampler_envelope_offset` | `declared_exact` | ui_sampler_envelope_bytes == size_of::<UiSamplerEnvelopeRegion>() |
| `ui_command_outcome` | `uiCommandOutcomeOffset` | `ui_command_outcome_offset` | `declared_exact` | ui_command_outcome_bytes == size_of::<UiCommandOutcomeRegion>() |

## Derived counts

- `C-OFFSET-REGIONS` = 25: Producer, Rust mirror, and validator descriptor populations have identical cardinality.
- `C-COMPARED-SPANS` = 26: The overlap pass receives every offset region plus the reserved mapping header.

## Rulings

- `R-HEADER-RESERVED` [DECIDED], dependencies `P-RESERVED-SPANS`: A region that aliases any byte of ShmHeader is refused; comparing only offset-defined regions is insufficient.
- `R-ZERO-LENGTH` [DECIDED], dependencies `P-VALIDATED-REGIONS`: A zero-length half-open interval overlaps nothing. The correct current audio_in/audio_out aliases at ring_std therefore pass without an exemption.
- `R-OFFSET-PRESENCE` [DECIDED], dependencies `P-VALIDATED-REGIONS`: Every one of the 25 v41 producer regions is allocated. A zero offset is corrupt geometry and makes attach fail; legacy accessor comments describing zero as optional do not weaken the exact v41 layout.
- `R-ALIGNMENT` [DECIDED], dependencies `P-VALIDATED-REGIONS`: Every region start is 64-byte aligned, matching the producer allocation cursor. Type alignment alone is not the layout invariant.
- `R-SIZE-SOURCES` [DECIDED], dependencies `P-VALIDATED-REGIONS`: Each size is checked from its declared scalar geometry, validated ring header, exact bytes companion, or Rust mirror type as listed in regions; no constant blanket size and no named overlap exemption is allowed.
- `R-RING-ORDER` [DECIDED], dependencies `R-SIZE-SOURCES`: A ring header must individually fit and validate before its span is computed; no ring pointer is retained or exposed until the later whole-layout overlap pass succeeds.
- `R-INTERVAL-ALGORITHM` [DECIDED], dependencies `R-ZERO-LENGTH`, `R-SIZE-SOURCES`: Build checked half-open spans, discard only zero-length spans from overlap comparison, sort by start then end, and refuse when a span starts before the preceding maximum end.
- `R-CACHED-GEOMETRY` [DECIDED], dependencies `R-INTERVAL-ALGORITHM`: Validation and use consume the same cached geometry. Typed accessors never reread region offsets or byte companions from mutable shared memory after attach.
- `R-FAIL-CLOSED` [DECIDED], dependencies `R-HEADER-RESERVED`, `R-OFFSET-PRESENCE`, `R-ALIGNMENT`, `R-RING-ORDER`, `R-INTERVAL-ALGORITHM`, `R-CACHED-GEOMETRY`: Any zero or misaligned offset, overflow, impossible exact-size companion, ring-shape failure, out-of-bounds span, header alias, or non-empty region overlap makes attach return Err; it is never degraded to a missing optional view.

## Implementation decisions

- `D-ATTACH-TRANSACTION` [READY], dependencies `R-FAIL-CLOSED`: Copy geometry into an untrusted descriptor after magic/version, validate the complete layout, cache the validated offsets/sizes, then construct EngineHandle's typed pointers from that cache. Dynamic header fields remain live; geometry does not.
- `D-NO-WIRE-CHANGE` [READY], dependencies `DEP-FROZEN-BASE`: The implementation changes validation only: K_SHM_VERSION remains 41 and both wire structs remain byte-identical.
- `D-OFFSET-ASSERTS` [READY], dependencies `D-NO-WIRE-CHANGE`: Add the six missing Rust offset assertions for audio_in, audio_out, ui_automation, ui_automation_slot, ui_sampler_kit, and ui_sampler_envelope; size/alignment equality alone does not prove field order.
- `D-TEST-MATRIX` [READY], dependencies `R-FAIL-CLOSED`: Add a correct-layout control plus deterministic zero-offset, 8-but-not-64-aligned, header-alias, fixed/fixed alias, ring/variable alias, zero-sized audio aliases accepted, the same audio alias made non-zero and refused, adjacency, audio-arithmetic-overflow, aligned-out-of-bounds, post-validation header mutation, and missing-descriptor controls. Table-drive wrong-byte mutations over every declared_exact region. Separately mutate inactive-ring nonzero geometry, event-ring zero/non-power-of-two/wrong-entry geometry, edit-ring wrong-capacity/wrong-entry geometry, and a truncated ring header.

## Controls

- `CTRL-BASE` [EXECUTABLE], dependencies `DEP-FROZEN-BASE`: python3 tools/architecture/ae_p1_3_packet_check.py verifies program and frozen commit/tree identities and that every cited governed product path matches the pin.
- `CTRL-CENSUS` [EXECUTABLE], dependencies `P-PRODUCER-OFFSETS`, `P-RUST-OFFSETS`, `P-VALIDATED-REGIONS`: python3 tools/architecture/ae_p1_3_packet_check.py derives all three populations and refuses additions, removals, duplicates, renames, and specifically omission of ui_command_outcome.
- `CTRL-PACKET` [EXECUTABLE], dependencies none: python3 tools/architecture/ae_p1_3_packet_check.py parses every source locator, validates complete gate dependency closure, checks counts and source pins, and enforces byte-identical generated prose.
- `CTRL-MUTATIONS` [PLANNED], dependencies `D-TEST-MATRIX`: cargo test -p daw-bridge layout_non_overlap executes every named positive and corrupt-header mutation after implementation, including a table-driven mutation for each declared_exact region and the distinct inactive/event/edit ring classes.
- `CTRL-STATIC` [PLANNED], dependencies `D-ATTACH-TRANSACTION`, `D-NO-WIRE-CHANGE`, `D-OFFSET-ASSERTS`: tools/ui_shm_layout_check.py derives the 25 producer assignments, 25 Rust fields, and 25 validator entries; it proves every typed accessor uses cached validated geometry and that attach validates before publication. Complete SHM offset assertions prove no wire change.

## Non-goals

- No SHM version or byte-layout change.
- No producer layout rewrite.
- No host-process shared-memory validation change.
- No protocol fingerprint work; that remains allocated to AE-P1.2.

## Review requirement

Implementation is authorized only after independent semantic and evidence reviewers both return PASS for the same immutable packet SHA and frozen product base.
