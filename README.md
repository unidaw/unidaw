# Uni

A digital audio workstation. A C++ engine hosts VST3 plugins out of process, has its own sampler,
plays a tracker-style note model over a harmony timeline, and renders offline deterministically.
The interfaces are a browser UI and a command-line client, both over shared memory. A personal
tool, built by one person with AI agents. No releases.

---

## Quick start

**To use it rather than build on it, read [docs/MANUAL.md](docs/MANUAL.md).**

Assuming JUCE at `$HOME/src/juce/JUCE` and Boost installed:

```sh
cmake -S . -B build && cmake --build build      # C++ engine, host, tests
cd ui && cargo build && cd ..                    # bridge, CLI, sidecar
./build/juce_scan --out build/plugin_cache.json --paths /Library/Audio/Plug-Ins/VST3

tools/webstack.sh > /tmp/stack.out 2>&1 &        # engine + sidecar + page server
open http://127.0.0.1:8173/index.html
```

To check it works:

```sh
ctest --test-dir build --output-on-failure       # 93 tests
```

Before changing code, read **Design commitments** at the bottom.

---

## What it is made of

Three kinds of process, and one shared-memory contract between each pair.

**`daw_engine`** owns transport, tempo and time-signature maps, harmony timeline, clip/placement
store, undo, device chains, patcher runtime, sampler and master mix. A `producer` thread schedules
events ahead of the playhead; the audio callback mixes finished blocks by block id and never
blocks — a late worker costs one stale frame, never a dropout.

It builds as two static libraries and a `main`:

- **`engine_core`** — the shared translation units: project file, event rings, shared memory,
  plugin host controller, patcher graph.
- **`engine_commands`** — 16 `apps/engine_*_commands.cpp` modules, one per UI command family
  (sampler, automation, clip, patcher, chain, track, marker, undo, …), each behind an explicit
  dependency struct rather than an implicit capture; plus `engine_pure` (rules that depend on
  nothing) and `engine_rt_helpers` (the producer thread's arithmetic).
- **`apps/daw_engine_main.cpp`** (~15.4k lines) — argument parsing, state, thread bodies, and the
  dispatch that routes a command to its module.

The split is what makes a command testable without booting a process: an edit-build-run cycle on a
command module is ~4s against ~14s to relink the engine. `apps/engine_types.h` holds the engine's
own types — `TrackRuntime` and the rest — which were function-local until they had to be shared.

**`juce_host_process`** is the plugin host: one process per track, spawned by the engine, over a
unix socket plus a per-track shared-memory segment carrying audio block rings and event queues. A
crashing plugin takes down its own track's host and nothing else. JUCE is confined to
`platform_juce/`.

**The UI** is `ui-web/` — plain HTML, CSS and JavaScript, no framework, no build step — served by
`daw-sidecar`, a Rust process that maps the engine's UI segment read-only and forwards it over
WebSockets (state on one socket, commands on another). `ui/daw-cli` is the same control surface on
the command line. Both write the same versioned command ring.

Timing is in **nanoticks**, 960,000 per quarter note. No floats in the note store.

---

## Platform and prerequisites

macOS (Apple Silicon) only. The shared memory is POSIX (`shm_open`/`mmap`), the real-time thread
hints in `apps/rt_thread.h` are macOS-specific, and audio device and VST3 hosting come from JUCE.

- CMake 3.18+, a C++17 compiler
- **JUCE**, expected at `$HOME/src/juce/JUCE` (override with `-DJUCE_DIR=...`)
- **Boost** headers — `property_tree` for JSON, `Boost.Graph` for the patcher DAG
- **Rust** / cargo — the bridge, the CLI, the sidecar, and the patcher kernels
- **Node** — only for `ui-web/` and its tests (no bundler; devDependency is Playwright)

---

## Build

```sh
cmake -S . -B build
cmake --build build
```

The build tree is `RelWithDebInfo`. Test targets get `-UNDEBUG` explicitly; several test mains
assert with `assert()`, which `NDEBUG` compiles away.

CMake also drives `cargo build --release` for `patcher_rust`, the Rust DSP/event kernels linked
into the engine. `-DDAW_BUILD_PATCHER_RUST=OFF` skips them; they are weak symbols, so the engine
still links and runs, and patcher nodes do nothing.

Then the Rust workspace:

```sh
cd ui && cargo build
```

Targets worth knowing: `daw_engine`, `juce_host_process` (the real host), `juce_scan` (plugin
scanner), `daw_lint` (project linter, shares the engine's parser), `identity_plugin` (a VST3 built
in-repo as a test fixture), and `juce_host` (a standalone one-plugin diagnostic — *not* what the
engine spawns).

`juce_host_process` is a separate target. Building only `daw_engine` after a contract change
leaves a host on the old layout; the engine refuses at startup and `ctest` catches it first (see
*contract freshness* below).

---

## Run

Scan for plugins once:

```sh
./build/juce_scan --out build/plugin_cache.json --paths /Library/Audio/Plug-Ins/VST3
```

**Full stack (engine + sidecar + page server):**

```sh
tools/webstack.sh > /tmp/stack.out 2>&1 &
open http://127.0.0.1:8173/index.html
```

Starts one engine, one sidecar and one page server on their own shared memory segment under an
exclusive lock, and refuses rather than guessing. Ports follow the page port: 8173 page, 8174
state, 8175 commands.

**Engine alone.** Run it *from* `build/` — it spawns `./juce_host_process` relative to the
working directory:

```sh
cd build && DAW_PROJECT_DIR=/tmp/proj ./daw_engine --run-seconds 10
```

**Drive it from the command line.** `ui/daw-cli` attaches to a running engine's segment
(`DAW_UI_SHM_NAME`, default `/daw_engine_ui`):

```sh
daw-cli get transport
daw-cli get tracks
daw-cli do load mysong
daw-cli do note --track 0 --nanotick 0 --pitch 60
daw-cli do play
```

`get` has queries for `transport tracks notes clip meters extents arrangement patcher
device-params automation automation-points sampler-kit waveform audio-sources diffs`; `do` covers
roughly sixty commands across transport, notes, placements, markers, mixer, routing, devices,
modulation, patcher and sampler. `daw-cli help` prints all of them. `--force` is accepted and
ignored.

**Offline render.** No wall clock: the engine pumps blocks as fast as the hosts finish them,
faster than real time, and cannot drop out.

**It does not need a working audio device to RUN, but it takes its sample rate from the default
output device unless you say otherwise.** Connect Bluetooth headphones and a bounce that was
44.1 kHz becomes 48 kHz, with nothing in the command to say so — which once failed a check that
had passed for weeks, on an engine that was entirely correct. Pass `--sample-rate` for a render
that means the same thing on any machine.

```sh
cd build && DAW_PROJECT_DIR=/tmp/proj \
  ./daw_engine --project mysong --render take1 --run-seconds 8 --sample-rate 48000
# writes /tmp/proj/take1.wav
```

`--project` is mandatory with `--render`. `--block-size N` forces the block grid, which is how
block-size invariance is checked end to end.

**Environment.** The ones that matter:

| Variable | Effect |
|---|---|
| `DAW_UI_SHM_NAME` | The UI segment name, and the seed for every per-track socket and segment name. Two engines that share it will corrupt each other. |
| `DAW_PROJECT_DIR` | Where projects, `.uni` modules, renders and `history.jsonl` live. |
| `DAW_EVENT_LOG` | Append one JSON object per engine decision to this path. Query it instead of grepping prose. |
| `DAW_CAPTURE_WAV` / `DAW_CAPTURE_SECONDS` | Record the master output during a real-time run; written at shutdown. |
| `DAW_PLUGIN_CACHE` | Where the engine reads `plugin_cache.json`. |
| `DAW_ENGINE_TEST_MODE=1` | Headless, no audio device, synthetic tracks. |
| `DAW_ENGINE_NUM_BLOCKS`, `DAW_ENGINE_BUFFER_SIZE`, `DAW_ENGINE_RENDER_THREADS` | Pipeline depth, device buffer request, render-pool size. |

---

## Test

```sh
ctest --test-dir build --output-on-failure     # 93 tests
```

`contract_freshness` is registered first; run and read it first. A stale binary makes every
failure below it point at the wrong subsystem.

Beyond ctest there are **86 end-to-end shell checks** in `tools/*_check.sh`, 42 of them wired into
ctest. The rest need a real audio device, a real plugin, or several minutes:

```sh
tools/all_checks.sh              # everything, sequentially
tools/all_checks.sh sampler kit  # only checks whose name matches a pattern
```

Pass/fail comes from the exit code, never from output text. Exit 2 means a prerequisite is missing
and is reported as SKIP, with a count. A check that fails and then passes on retry is reported
FLAKY, never PASS. Per-check logs land in `build/check-logs/`. Never run them in parallel; several
open the audio device.

Rust and the web UI have their own:

```sh
cd ui && cargo test                          # bridge layout, agent, CLI, engine e2e
cd ui-web && npm test                        # unit + screenshot goldens + allocation budget
tools/verify.sh                              # rust + js units + goldens + allocation
tools/verify.sh --engine                     # also every suite that drives a real engine
```

---

## Layout

```
apps/                 the C++ engine, the plugin host, the contracts, and the test mains
platform_juce/        the only place JUCE is used (hosting, audio device, file decode)
patcher_rust/         Rust kernels for patcher nodes, linked via the C ABI in apps/patcher_abi.h
plugins/identity/     a VST3 built in-repo, used as a test fixture
ui/daw-bridge/        the Rust mirror of the shared-memory contract
ui/daw-cli/           the command-line client
ui/daw-sidecar/       SHM to WebSocket, and the server for the web UI
ui/daw-agent/         perception + tool substrate over the engine, model-agnostic
ui-web/               the web UI (no framework, no build step) and its test suites
tools/                the end-to-end checks, the shared wait library, perceptual.py
presets/              fixture projects, patcher presets and audio
docs/                 design documents
```

Key files:

- `apps/shared_memory.h` — the engine↔UI contract. `kShmVersion` is **36**; the comment block above
  it is the version history and says what each bump bought.
- `apps/event_payloads.h` — every UI→engine command. `UiCommandType` runs to **90**, next free 91.
- `apps/project_file.cpp` — the project format, schema version 4.
- `apps/placement_flatten.h` — the one definition that derives the flat clip.
- `tools/lib/engine_wait.sh` — the boot/load wait library 36 checks share, with its own self-test.

---

## The musical model

**Notes live in clips; clips live in a project-level library; tracks reference them through
placements.** A placement carries an absolute position, a length (a shorter clip loops to fill it)
and additive-only, one-level overrides: `adds` are notes belonging to that appearance alone,
`mutes` silence base notes by id. The flat event list the scheduler plays is **derived** from
(placements + clips) by `flattenPlacements` after every edit; nothing edits it directly. Undo is
a whole-track store swap.

**Row ops** are the tracker effect column, typed and named rather than packed into hex.
Space-separated, order-free; a malformed token is a hard parse error, not a silent no-op:

| Token | Meaning |
|---|---|
| `ret3` | retrigger — N even re-strikes across the note |
| `rv-60` | retrigger volume ramp, signed total percent across the burst |
| `p60` | probability, deterministic per note id — the same note decides the same way every render |
| `d1/6` | onset delay as a fraction of a beat |
| `s7` | sound address — which sampler slot plays, independent of pitch |
| `o80` / `o1/3` | sample start offset as a fraction of the slot's extent, so it survives a re-chop |
| `c1:2` | conditional trig — fire on pass A of every B |
| `cpre` / `cnpre` | fire if the previous conditional on this track did / did not |

**Harmony is a timeline, resolved per note at its own tick.** A `HarmonyEvent{nanotick, root,
scaleId}` is a global key change; `harmonyAt` returns the context in force at any tick, and a note
resolves against it before being emitted. On top of it:

- **Degree notes** — stored as a scale degree rather than a pitch. Transpose the key and it follows.
- **Chords** — `degree + quality + inversion` expands to up to four pitches at schedule time
  (`apps/chord_resolver.h`). Stored as the chord you meant, not the notes it became.
- **Scale quantize** — per track, snapping played pitch to the active scale. **Off by default**:
  the map is many-to-one, so a quantize you cannot see cannot be undone from the result.

**Tuning is carried in cents all the way to the plugin.** Pitches resolve to `{midi, cents}`, the
cents ride the note event, and the host puts them on the VST3 note-on and note-off as per-note
tuning. `UiScale` carries up to 48 steps per octave in milli-cents; `Interval` holds an exact
frequency ratio alongside its cents. The pipeline is microtonal end to end; the content is not.
`ScaleRegistry` ships four hardcoded 12-TET modes (Major, Minor, Dorian, Mixolydian) and there is
no scale import, so every resolution today comes back at zero cents.

**The sampler** (`DeviceKind::Sampler`) is an in-engine device, not a plugin: it renders into the
host input plane ahead of the chain, so VST effects can follow it on the same track. Multi-sample
kits with key and velocity zones, round-robin and random selection; transient detection and
chopping with stable slice ids, so a chop can be re-cut while it plays; loops (forward, ping-pong,
backward) with a crossfaded seam; an octave mip-map and three interpolation qualities. Filters,
multipoint envelopes and LFOs live on a shared **mod set** that slots point at. Per-slot stem
outputs become child tracks. Full design: `docs/SAMPLER_DESIGN.md`.

**The patcher** is a per-device node graph; there is no track-level or global patcher. Euclidean
synthesises rhythm from nothing, RandomDegree rewrites a gate's pitch, SliceSelect chooses which
sample a gate plays, LFO drives modulation. Generators touch gates only; a note you wrote with a
pitch stays the note you wrote.

**Projects** are `<name>.uniproj.json` — canonical JSON with fixed key order, written atomically,
so an unchanged document re-saves byte-identically. A `.uni` file is the same document packed as a
zip with its samples inside (stored, not deflated) for moving between machines; the loose form is
not replaced.

---

## Design commitments

**A render is a function of the document.** Asserted byte for byte, never with a tolerance:

- Two offline renders of one project are byte-identical.
- Renders at 64, 256 and 1024 frames are byte-identical over the common length.
- The same render on one thread and on many is byte-identical.

(`tools/sampler_determinism_check.sh`, `tools/offline_render_check.sh`,
`tools/render_pool_check.sh`, `tools/slice_select_check.sh`.)

**The version gate is hard equality.** `kShmVersion` must match exactly on attach; a mismatched
client is refused, on both the C++ and Rust sides. No forward compatibility, no best-effort read.
Any struct change bumps the version. The project format is the opposite: it accepts older schema
versions and migrates them.

**The Rust mirror is generated, not typed.** `ui/daw-bridge/build.rs` runs bindgen over
`apps/shared_memory.h` and `apps/event_payloads.h`. Hand-written mirrors are ratcheted against the
generated twins by `layout.rs::bindgen_matches_hand_written`, and `tools/contract_layout_check.sh`
derives both sets from source and fails if anything in the intersection is unlisted. Field
reordering within an identical size is still uncovered.

**Derived, never stored twice.** The flat clip comes from placements and clips, slice extents from
marker order, ADSR from the same envelope points, bar positions through the meter map.

**Negative controls, or the check does not count.** Nearly every check drives a real engine and
asserts on real audio or on the saved file; its header names the specific defect it prevents and
why the obvious version of the check would have passed with that defect present. A fix is
validated by reverting it, confirming the check fails *with the right message*, and restoring.
Sample headers: `tools/sampler_filter_check.sh`, `tools/lint_check.sh`,
`tools/sampler_edit_while_playing_check.sh`, `tools/clip_anchor_meter_check.sh`.

---

## Documents

`AGENTS.md` is the working agreement for agents in this repo. Parts of its lower half predate the
web UI and are stale (it still cites `kShmVersion` 15 and 31 tests); the environment section at
the top is maintained.

| Document | What it is |
|---|---|
| **`docs/MANUAL.md`** | **The user manual.** How to work the program, for someone who already knows trackers and DAWs. Written against the source, so it names what refuses as well as what works. |
| `docs/SAMPLER_DESIGN.md` | The sampler: decisions, requirements, and what shipped. Current. |
| `docs/row-ops.md` | The typed effect column. Predates most of the ops — `ui/daw-bridge/src/rowop.rs` is the authority. |
| `docs/per-lane-grids.md` | Per-lane row subdivisions. Model and projection built; UI work still owed. |
| `docs/TRACKER_GAP_LIST.md` | Ranked survey of classic-tracker features, with an explicit "not worth it" section. Written before the sampler landed; framing is stale, rulings are not. |
| `SHM_LAYOUT.md` | The segment layout and the UI seqlock protocol. |
| `PROJECT_PERSISTENCE.md` | The project/module format. Specifies a zip container as the project form; what shipped is the loose JSON plus `.uni` as the packed form. |
| `PATCHER.md` | The patcher ABI and FFI boundary. Architecture is accurate; the payload and node listings have drifted. |
| `DEVICE_CHAIN.md`, `DEVICE_CHAIN_PATCHER_UX.md` | The dual-rail chain model and its UX. |
| `MASTER_TRACK_DESIGN.md` | The master track as a patcher-carrying device chain. |
| `ARCHITECTURE_REVIEW.md` | A dated review (2026-07-24) that set the current direction. Several defects it catalogues have since been fixed. |
