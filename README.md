# Uni

Audio workstation: tracker-first UI, first-order harmony, microtonal support, patcher DAG for events & DSP, VST3 hosting; planned piano roll, mixer, and arrangement views.

Status: very early. Do not use.

## Components
- Engine (C++): scheduling, harmony, clip edits, audio graph, IPC.
- Plugin host (C++/JUCE): VST3 scan/instantiate/process, state, editor windows.
- UI (Rust/GPUI): tracker, palette, scale browser, SHM snapshot reader.
- Patcher (C++ + Rust bridge): event/control graph with deterministic DAG.

## Build
- Build system: CMake for C++ targets; Cargo for Rust UI.
- Example host binary: `build/juce_host`.
- UI app: `ui/daw-app` (spawns `build/daw_engine` by default; override with `DAW_ENGINE_PATH`).
- C++ build (JUCE defaults to `$HOME/src/juce/JUCE`; override with `-DJUCE_DIR=...`):
```sh
cmake -S . -B build
cmake --build build
```
- Rust UI build:
```sh
cd ui
cargo build -p daw-app
```
- Start UI (spawns `build/daw_engine` by default):
```sh
cd ui
cargo run -p daw-app
```

## Tests
- C++: `ctest --output-on-failure` (Phase 2 + Phase 3 suites).
- Patcher: `ctest -R patcher_ --output-on-failure`.
- UI integration: `cargo test -p daw-app --test engine_integration`.

## IPC and shared memory
- UI reads engine snapshots from shared memory; protocol details in `SHM_LAYOUT.md`.
- Control-plane messages use fixed headers and typed payloads; see `apps/ipc_protocol.h`.

## Repository layout
- `apps/`: engine, host, IPC, shared memory, scheduling, tests.
- `platform_juce/`: JUCE wrappers and utilities.
- `plugins/`: internal test plugins.
- `patcher_rust/`: Rust patcher bridge.
- `ui/`: Rust UI workspace (bridge, CLI, app).

## Notes
- Timing is in nanoticks (960,000 per quarter note).
- Harmony is applied before note dispatch; microtonal tuning uses VST3 per-note cents.
