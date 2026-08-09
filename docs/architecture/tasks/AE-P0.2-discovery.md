# AE-P0.2 discovery — Isolated run context and provenance

State: `READY FOR READ-ONLY ASSIGNMENT`

Discovery owner: `claude-worker-2`

Independent reviewer: `codex-worker-2`

## Outcome

Produce an evidence-backed design and migration map for AE-P0.2. It must identify
every primary place a build or test selects source, binaries, Cargo targets,
dependencies, ports, shared-memory names, project data, logs, captures, or
temporary/evidence paths. It must then propose one coherent run-context contract
and machine-readable provenance manifest that lets two worktrees build and test
concurrently without artifact substitution or runtime collision.

This is discovery only. It does not authorize edits, builds, tests, branches,
worktrees, commits, cleanup, package installation, or process launches.

## Baseline and location

- Frozen product baseline: `5bef283798b59c2c4f5720292554c7ab8c265be6`.
- Read-only checkout: `/Users/jak/src/daw-backend`.
- The architecture branch may gain governance-only commits while discovery runs;
  report the exact HEAD inspected and distinguish product baseline from later
  task-record commits.

## Dependencies

- AE-P0.1 is active and owns checkout-root resolution. Treat its packet and
  owned files as a boundary, not as a surface to redesign concurrently.
- The clean baseline build succeeded, but the canonical `build/` refresh and
  full baseline suites are still running.
- AE-P0.2 implementation remains blocked until AE-P0.1 integrates and this
  discovery has independent review.

## Owned files

None. This assignment is strictly read-only.

## Required read-only context

- `ARCHITECTURE_EXCELLENCE_PLAN.md`, especially AE-P0 and AE-P0.2
- `ARCHITECTURE_EXCELLENCE_LEDGER.md`
- `docs/architecture/tasks/AE-P0.1.md`
- root `CMakeLists.txt` and generated CTest metadata
- `.gitignore`, Cargo workspace manifests/config, npm manifests
- primary verification scripts under `tools/`
- `ui/daw-agent/tests/engine_e2e.rs`
- `ui-web/test/{all,stack,serve,e2e}.mjs` and shared test helpers

## Questions the report must answer

1. Which commands use their current checkout, and which substitute
   `<source>/build`, a sibling tree, a shared Cargo target, or stale binaries?
2. Which CMake/CTest tests honor `CMAKE_BINARY_DIR` or `DAW_BUILD_DIR`, and which
   ignore it? Give machine-derived counts and exact high-risk examples.
3. What writable paths are shared or fixed: build products, Cargo targets,
   `node_modules`, plugin cache, ports, SHM names, Unix sockets, pid/lock files,
   projects, journals, captures, logs, and failure evidence?
4. Where are PID-derived names sufficient for simultaneous processes, and where
   can PID reuse, stale resources, fixed prefixes, or time-of-check/time-of-use
   port allocation cause false isolation?
5. Which tests require real audio/device serialization, and which are serialized
   only because their resource model is incomplete?
6. How should one immutable `RunContext` be created, validated, serialized, and
   consumed across CMake/CTest, shell, Rust, and Node without four hand-written
   mirrors?
7. Which fields and hashes belong in the provenance manifest so results fail
   closed on source/artifact/dependency mismatch?
8. How should JUCE, Boost, Rust crates, npm packages, compiler/SDK, protocol
   version, build configuration, and dirty state be identified reproducibly?
9. What is the smallest safe migration order, with file ownership slices that do
   not overlap AE-P0.1 or protocol merge hotspots?
10. What executable two-worktree and artifact-swap negative controls prove the
    design rather than merely exercise its happy path?

## Constraints

- Do not propose copying source files between worktrees or separate Git object
  stores; these are Git worktrees sharing one repository object database by
  design. Isolation concerns writable checkout/build/runtime resources.
- Do not make a PID, fixed port, current CWD, `$HOME`, or unchecked environment
  variable the authority for identity or provenance.
- Do not solve provenance by embedding machine-specific absolute paths in
  tracked files.
- Do not require secrets, paid services, network access, or a real plugin merely
  to validate run-context plumbing.
- Preserve explicit serialization for the physical audio device until an
  evidence-backed substitute exists.
- Separate immediate P0 gates from broader Phase 5 build-system cleanup.

## Evidence and method

Use read-only commands such as `rg`, `git ls-files`, `git worktree list`, CTest
`-N`/`--show-only=json-v1`, and manifest/cache inspection. Do not execute a test
or build as part of discovery.

The handoff must include:

- exact inspected HEAD and source realpath;
- tables mapping each resource to current owner/derivation/collision risk;
- machine-derived counts with commands sufficient to reproduce them;
- proposed `RunContext` schema and validation rules;
- proposed provenance-manifest schema and fail-closed checks;
- migration slices with owned files, dependencies, and independent tests;
- negative controls including two simultaneous worktrees and deliberately
  swapped source/build/artifact identities;
- unresolved architectural decisions, if any, stated as ADR questions rather
  than silently chosen defaults.

## Review focus

`codex-worker-2` will independently challenge:

- inventory completeness and reproducibility of counts;
- hidden writable global state and stale-resource cases;
- whether the proposed identity survives parallel processes and restarts;
- whether provenance proves artifact origin rather than printing plausible text;
- cross-language schema duplication;
- scope separation from AE-P0.1 and Phase 5; and
- whether negative controls would fail a deliberately contaminated baseline.

## Stop and escalate

Stop if discovery would require a write, process launch, package install, secret,
network request, sibling-worktree modification, or architectural choice outside
the questions above. Report the blocker with the exact missing evidence.
