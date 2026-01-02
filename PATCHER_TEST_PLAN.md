# PATCHER_TEST_PLAN.md

This document defines the test plan for the Patcher subsystem described in
`PATCHER.md` and implemented per `PATCHER_IMPL_PLAN.md`.

## Goals

- Verify deterministic event generation and ordering.
- Verify correct MusicalLogic -> MIDI resolution and note-off scheduling.
- Verify stable sorting priorities and `priority_hint` handling.
- Verify RT safety constraints (no allocation/logging on audio thread).
- Verify overflow behavior and async logging path.

## Test Categories

### 1) Rust Kernel Unit Tests (Euclidean Trigger)

Purpose: validate deterministic pattern generation and event emission.

Tests (post-resolution event rail only; no MusicalLogic entries should remain):

- Fixed pattern generation
  - Inputs: hits=5, steps=16, offset=0, block_start_tick=0
  - Expect: MusicalLogic events at exact step boundaries for the Bjorklund
    pattern (precomputed reference list).
- Offset handling
  - Inputs: hits=5, steps=16, offset=3
  - Expect: events shifted by the offset in loop-local step space.
- Duration
  - Inputs: duration_ticks = fixed value (e.g. 120)
  - Expect: all MusicalLogic events set `duration_ticks` correctly.
- Overflow
  - Inputs: event_capacity small (e.g. 4) with enough hits to exceed capacity
  - Expect: events beyond capacity are dropped, and `last_overflow_tick` updated.

### 2) Resolution Pass Tests (C++)

Purpose: validate MusicalLogic -> MIDI conversion and note-off insertion.

Tests:

- Degree -> MIDI conversion
  - Inputs: MusicalLogic degree + harmony snapshot (known scale/root)
  - Expect: MIDI pitch matches resolved scale degree for the given root.
- Note-off insertion
  - Inputs: MusicalLogic event with duration_ticks > 0
  - Expect: note-off event inserted at `nanotick + duration_ticks`.
- Preserve metadata
  - Inputs: MusicalLogic with priority_hint/metadata
  - Expect: priority_hint used for ordering, metadata ignored by resolver.

### 3) Sort Order Tests (C++)

Purpose: validate deterministic ordering with computed priorities.

Tests:

- Same nanotick ordering (post-resolution)
  - Inputs: Transport, Param, MIDI NoteOff, MIDI NoteOn events all at the same
    nanotick
  - Expect: order is Transport -> Param -> NoteOff -> NoteOn.
- MusicalLogic resolution ordering
  - Inputs: multiple MusicalLogic events at same nanotick with differing hints
  - Expect: resolution pass converts to MIDI in ascending `priority_hint` order;
    final sort preserves that order among NoteOns at the same nanotick.
- Stability
  - Inputs: identical events with identical keys in stable order
  - Expect: stable_sort preserves original order.

### 4) Deterministic Multithreading Tests

Purpose: validate bit-identical output across differing thread schedules.

Tests:

- Parallel DAG order
  - Inputs: two independent nodes (A, B) emitting events at the same nanotick
  - Execution: run multiple times with different thread scheduling
  - Expect: merged event list is identical, ordered by topological order.

### 5) RT Safety Tests

Purpose: ensure no RT violations in audio path.

Tests:

- Allocation guard (optional)
  - Instrument or assert that no heap allocations occur in the audio callback
    and kernel processing path.
- Logging guard
  - Ensure overflow logging is performed off the audio thread only.

### 6) Alignment and ABI Tests

Purpose: ensure FFI layout compatibility across C++ and Rust.

Tests:

- Static asserts in C++ for `EventEntry` and `MusicalLogicPayload` size/alignment.
- Rust compile-time checks (`mem::size_of`, `mem::align_of`) for ABI mirrors.

## Exit Criteria

- All unit and integration tests pass.
- Deterministic output verified under parallel execution.
- No RT safety violations detected in audio thread path.
- ABI layout checks green on all target platforms.
