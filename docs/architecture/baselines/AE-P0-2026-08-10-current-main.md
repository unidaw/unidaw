# AE-P0 current-main rebaseline — 2026-08-10

State: `BUILD COMPLETE; RUNTIME GATES NOT RUN`

Product authority: `/Users/jak/src/daw`, `main`

Product SHA: `62bafdc6cf1cd53168ce73d098cd6acc78659be8`

Governance record: `4b0385e8746b582dd1a1c05da051fbc8420bc930` on the separate
`architecture-audit` line. The governance line is not treated as product source;
its merge base with the product is historical `5bef283798b59c2c4f5720292554c7ab8c265be6`.

## Configure and inventory authority

Fresh metadata-only configure was completed from the product checkout into
`/Users/jak/src/daw/build-ae-current` with `RelWithDebInfo` and
`DAW_BUILD_PATCHER_RUST=ON`. The configure completed at 2026-08-10 01:13:23
(Europe/Helsinki), after the selected product SHA and its last `CMakeLists.txt`
change. A subsequent full compile-only build completed successfully at
01:23:41; no test or runtime process was launched for this baseline.

| Population | Count |
|---|---:|
| configured CTest tests | 212 |
| override-immune `BUILD="$ROOT/build"` | 103 |
| override-aware `DAW_BUILD_DIR` | 15 |
| bare-CTest substituted union | 118 |
| other compiled/Cargo/entry points | 94 |
| tests with `WORKING_DIRECTORY` | 212/212 |
| `RUN_SERIAL` | 2 |
| `RESOURCE_LOCK` | 0 |
| `tools/*.sh` including `tools/lib/` | 156 |
| `BUILD="$ROOT/build"` shell assignments | 106 |
| `BUILD="${DAW_BUILD_DIR:-$ROOT/build}"` shell assignments | 18 |
| `BUILD="$ROOT/build-tsan"` | 1 |
| non-comment `./daw_engine` launchers | 123 |
| scripts passing `--no-audio` | 1 |

The 118 substitution union remains exactly equal to the independently derived
engine-launching CTest set. The 5bef283 historical evidence reported 211/103/15/93
and top-level-only shell population 153; the current configured tree adds one
CTest entry and three scripts, without changing the substitution structure.

## Device authority

The current product still has three direct default-device callers:

- `apps/audio_probe_main.cpp`
- `apps/engine_startup.cpp`
- `apps/juce_host_main.cpp`

All funnel into `platform_juce/juce_wrapper.cpp`; no lease exists yet. Physical
device serialization is therefore not proven by the two `RUN_SERIAL` tests and
remains a release-blocking AE-P0 gate.

## Artifact and runtime status

Before the isolated build completes, existing `build/` and Rust CLI artifacts
predate `62bafdc`; they are not evidence for this baseline. The current-main
build is intentionally isolated under `build-ae-current`. No CTest, Rust e2e,
web suite, engine, host, sidecar, audio probe, or capture has been run from this
baseline record.

## Method corrections

The inventory includes both `tools/lib/engine_wait.sh` and
`tools/lib/load_generator.sh`; prior top-level-only shell glob evidence omitted
them. Counts use `LC_ALL=C` and line-safe iteration. A zero from a failed shell
word-splitting loop is not accepted as evidence.
