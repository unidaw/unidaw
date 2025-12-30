# PATCHER_IMPL_PLAN.md

This document captures the concrete implementation plan for the Patcher
subsystem as defined in `PATCHER.md`.

## Phase 1: ABI + Engine Plumbing

1) Add ABI header
- Create `apps/patcher_abi.h` with `MusicalLogicPayload`, `PatcherContext`,
  and the `EventType::MusicalLogic = 9` definition.
- Add `atomic_store_u64` shim declaration.
- Add static assertions for `EventEntry` size/alignment and payload size.

2) Engine integration (single-threaded path first)
- Add a fixed-capacity `EventEntry` scratchpad per track.
- Implement the resolution pass: `MusicalLogic` -> `Midi`, inject note-offs.
- Implement stable sort using tuple ordering with `priorityFor` and
  `priority_hint` (keep `priorityFor` as inline/constexpr switch).

3) Overflow tracking
- Add `last_overflow_tick` atomic storage in engine stats/state.
- Implement `atomic_store_u64` using `std::atomic_ref<uint64_t>` (relaxed).
- Add async poller/logger (non-RT) to surface overflow tick changes.

## Phase 2: Deterministic Multithreading

1) Node-local buffers
- Allocate fixed-capacity node-local buffers for each DAG node.
- Nodes write locally; no shared scratchpad writes in parallel.

2) Deterministic merge
- Cache topological order at graph-change time.
- Merge node-local buffers into track scratchpad in topological order.

3) Validation
- Deterministic output checks: same seed/time -> identical event list.
- Verify ordering invariants (note-off before note-on at same tick).

## Phase 3: Rust Kernel Integration

1) Rust crate skeleton
- Add a `staticlib` crate with FFI mirrors for `EventEntry`, `HarmonyEvent`,
  `PatcherContext`, and `MusicalLogicPayload`.
- Expose `extern "C"` kernel entrypoints.

2) Euclidean Trigger kernel
- Precompute Bjorklund pattern on parameter change.
- Emit `MusicalLogic` events with `degree`, `octave_offset`, `duration_ticks`.
- Use overflow policy: drop + `atomic_store_u64`.

## Phase 4: Test Harness

1) Deterministic kernel tests
- Fixed inputs for hits/steps/offset -> known event list.
- Verify note-off injection timing.

2) Engine regression tests
- Validate sort priorities and overflow handling under load.
- Confirm alignment/static_asserts on all supported platforms.

## Exit Criteria

- ABI header compiled cleanly on engine + Rust.
- Kernel emits stable events; resolution pass generates correct MIDI.
- Deterministic ordering holds under multi-threaded DAG execution.
- No RT violations (allocations/logging/locks) on the audio thread.
