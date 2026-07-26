# Uni

Audio workstation: tracker-first UI, first-order harmony, microtonal support, patcher DAG for events & DSP, VST3 hosting; planned piano roll, mixer, and arrangement views.

Status: very early. Do not use. Not possible to save. But tracker and patcher PoC is somewhat working.

## Components
- Engine (C++): scheduling, harmony, clip edits, audio graph, IPC.
- Plugin host (C++/JUCE): VST3 scan/instantiate/process, state, editor windows.
- UI: a web front-end driven by a Rust sidecar over the SHM snapshot (replaces the
  former Rust/GPUI `daw-app`, now removed). The in-app agent (`ui/daw-agent`) is the
  Rust-side perception + tool substrate the sidecar and automated harnesses share.
- Patcher (C++ + Rust bridge): event/control graph with deterministic DAG.

## Build
- Build system: CMake for C++ targets; Cargo for Rust UI.
- Example host binary: `build/juce_host`.
- Engine binary: `build/daw_engine` (spawns per-track `build/juce_host_process`; run it from `build/`).
- C++ build (JUCE defaults to `$HOME/src/juce/JUCE`; override with `-DJUCE_DIR=...`):
```sh
cmake -S . -B build
cmake --build build
```
- Rust build (bridge, CLI, in-app agent):
```sh
cd ui
cargo build
```
- Scan plugins (creates/updates cache):
```sh
./build/juce_scan --out build/plugin_cache.json --paths /path/to/VST3
```
- Drive a running engine (observe / edit) via the in-app agent:
```sh
cd ui
DAW_UI_SHM_NAME=/daw_engine_ui cargo run -p daw-agent --example observe
```

## Tests
- C++: `ctest --output-on-failure` (Phase 2 + Phase 3 suites).
- Patcher: `ctest -R patcher_ --output-on-failure`.
- Rust: `cargo test` in `ui/` (bridge layout/offset asserts, agent, CLI).

## IPC and shared memory
- UI reads engine snapshots and sends commands via shared memory rings; see `SHM_LAYOUT.md`.
- Engine/host control-plane messages use fixed headers and typed payloads; see `apps/ipc_protocol.h`.

## Repository layout
- `apps/`: engine, host, IPC, shared memory, scheduling, tests.
- `platform_juce/`: JUCE wrappers and utilities.
- `plugins/`: internal test plugins.
- `patcher_rust/`: Rust patcher bridge.
- `ui/`: Rust UI workspace (bridge, CLI, app).

## Notes
- Timing is in nanoticks (960,000 per quarter note).
- Harmony is applied before note dispatch; microtonal tuning uses VST3 per-note cents.
