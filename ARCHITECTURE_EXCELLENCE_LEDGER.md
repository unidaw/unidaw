# Architecture Excellence Execution Ledger

This is the mutable execution companion to
`ARCHITECTURE_EXCELLENCE_PLAN.md`. The plan defines architecture and gates; this
ledger records current orchestration state.

## Global state

```text
Program:       ACTIVE -- see the Ticket state table below; it is the single authority for phase status
Reason:        execution baseline selected at current main; old evidence remains historical
Target repo:   /Users/jak/src/daw-backend
Baseline SHA:  7710401d72029482c8f3d15869d58dce7e246def
Historical evidence SHA: 5bef283798b59c2c4f5720292554c7ab8c265be6
Worktrees:     AE-P0.1 integration landed in /Users/jak/src/daw at 71758c0; review worktree preserved at /Users/jak/src/daw-ae-p0-followup
Active tasks:  See the Ticket state table below; do not duplicate phase status or ownership in this summary
File locks:    protocol hotspots frozen; root CMake reserved narrowly for AE-P0.1
Integration:   AE-P0.1 and AE-P0.2 evidence is recorded in the Ticket state table and historical activation record
```

No worker may edit, build, test, create a branch/worktree, commit, or self-assign
architecture-remediation work without an explicit `ASSIGNED` task packet.

## Activation record

The owner-authorized activation trigger has been satisfied:

- `refactor` delivered `[UNDO][DONE]` for `main` at `5bef283798b59c2c4f5720292554c7ab8c265be6`.
- `main`, `origin/main`, and the handoff SHA matched before integration.
- The prior `architecture-audit` HEAD was a strict ancestor and was fast-forwarded
  without conflict to the frozen baseline.
- The plan and this ledger were committed as the governance bootstrap at
  `762fe34` before any remediation worktree was created.
- The first remediation worktree was created only after its complete task packet
  was committed.

## Bootstrap roster

| Handle | Kind | Initial pairing | State |
|---|---|---|---|
| `backend` | Codex | Orchestrator/integrator; current lane per Ticket state table | `ACTIVE` |
| `codex-worker-1` | Codex | Lane A implementation owner first | `COMPLETE: final repair chain delivered; original lane clean at 2f7aa93` |
| `claude-worker-1` | Claude | Lane A independent reviewer first | `APPROVED: e4e08de exact; wrapper-only successor 0f48f69 documented` |
| `claude-worker-2` | Claude | Lane B discovery owner first | `HOLD: evidence and current-main delta delivered` |
| `codex-worker-2` | Codex | Lane B independent reviewer first | `HOLD: awaiting baseline and exact ADR SHA` |

Implementation and review roles rotate after each integrated ticket. Pairing is
not authorization to begin a ticket.

## Activation checklist

- [x] Undo refactor declared complete by its owner.
- [x] Undo changes integrated.
- [x] Owner's conditional `GO` became effective on `[UNDO][DONE]`.
- [x] `/Users/jak/src/daw-backend` is clean except approved planning files.
- [x] Exact current product baseline selected; historical evidence remains
      explicitly attributable to the prior SHA.
- [x] Baseline build/test evidence captured; the machine-readable provenance
      mechanism remains the AE-P0.2 implementation gate.
- [x] All four workers acknowledge the target path, baseline, plan, and `HOLD`.
- [x] First-wave tickets have complete task packets.
- [x] Merge-hotspot ownership assigned before any worktree is created.
- [x] Every first-wave ticket has an independent cross-model reviewer.

## Worker acknowledgements

| Handle | Plan read | Target path confirmed | HOLD confirmed | Delivery registration | Last evidence |
|---|---:|---:|---:|---|---|
| `codex-worker-1` | yes | yes | yes | Codex Stop guard verified | `[AE-P0][ACK]` 2026-08-09 |
| `codex-worker-2` | yes | yes | yes | Codex Stop guard verified | `[AE-P0][ACK]` 2026-08-09 |
| `claude-worker-1` | yes | yes | yes | Claude delivery/PONG verified | `[AE-P0][ACK]` 2026-08-09 |
| `claude-worker-2` | yes | yes | yes | Claude delivery/PONG verified | `[AE-P0][ACK]` 2026-08-09 |

Orchestrator channel registration:

```text
handle:   backend
runtime:  Codex
session:  019fdc27-4c30-7dd2-846c-500108bbbf24
cwd:      /Users/jak/src/daw-backend
delivery: Stop guard and external wake verified end to end
watcher:  none (required for Codex)
```

## Ticket state

| Ticket | State | Dependency | Owner | Reviewer | Worktree | Commit |
|---|---|---|---|---|---|---|
| `AE-P0` | `ACTIVE` | current-main rebaseline + formal review | `backend` | unassigned | root | `62bafdc` execution baseline |
| `AE-P0.1` | `COMPLETE` | packet `258f423` + independent review | `codex-worker-1` | `claude-worker-1` | `/Users/jak/src/daw-ae-p0-followup` | product main `71758c0`; final chain ends `3b53a29` |
| `AE-P0.2 discovery` | `ESCALATED_TO_ADR` | frozen baseline + packet | `claude-worker-2` | `codex-worker-2` | read-only root | four rejected designs; evidence complete |
| `AE-P0.2 ADR` | `APPROVED` | current-main inventory + exact review | `backend` | `codex-worker-2` | root | exact SHA `7dff997`; approval received |
| `AE-P0.2 implementation` | `COMPLETE` | packet `6287ffd` approved + AE-P0.1 integration | codex-worker-2 | claude-worker-2 | `/Users/jak/src/daw-ae-p0-2-lane0` | product main `75c6f06`; final corrective candidate independently approved |
| `AE-P0.3` | `BLOCKED` | AE-P0.1 review + frontend ownership release | unassigned | unassigned | none | packet ready |
| `AE-P1.1` | `FROZEN` | `AE-P0` | claude-worker-2 | codex-worker-1 | `/Users/jak/src/daw-ae-p1-1-packet` | `ba88bcb4657b62bdfc752d338d877e139e212ca6`; independent PASS; successor-only |
| `AE-P1.2` | `ACTIVE` | `AE-P1.1` | claude-worker-2 | codex-worker-1 | `/Users/jak/src/daw-ae-p1-2-packet` | settled packet `78a1394eb2bd5c46b3ca064331bb91a67c294d96`; 19 open items; G4 not decidable |
| `AE-P1.3` | `BLOCKED` | `AE-P1.2` | unassigned | unassigned | none | none |
| `AE-P1.4` | `BLOCKED` | `AE-P0` | unassigned | unassigned | none | none |
| `AE-P1.5` | `BLOCKED` | `AE-P0` | unassigned | unassigned | none | none |
| `AE-P1.6` | `BLOCKED` | `AE-P0` | unassigned | unassigned | none | none |
| `AE-P2.*` | `BLOCKED` | Phase 1 gates | unassigned | unassigned | none | none |
| `AE-P3.*` | `BLOCKED` | Phase 1/2 contracts | unassigned | unassigned | none | none |
| `AE-P4.*` | `BLOCKED` | Phase 2 transactions | unassigned | unassigned | none | none |
| `AE-P5.*` | `BLOCKED` | replacement behavior gated | unassigned | unassigned | none | none |

## Merge-hotspot ownership

During AE-P0, `backend` is the sole integration owner for every merge hotspot.
Task-specific ownership is assigned before any Phase 1 worktree is created:

| Hotspot | Owner | Lock state |
|---|---|---|
| `apps/shared_memory.h`, generated wire headers, protocol version | `backend` | `FROZEN: AE-P0` |
| `apps/event_payloads.h`, command registry/schema | `backend` | `FROZEN: AE-P0` |
| `ui/daw-bridge/src/layout.rs`, generated Rust wire types | `backend` | `FROZEN: AE-P0` |
| `ui/daw-bridge/src/control.rs` | `backend` | `FROZEN: AE-P0` |
| `apps/engine_types.h`, `apps/daw_engine_main.cpp` | `backend` | `FROZEN: AE-P0` |
| root CMake/test registration | `codex-worker-1` | `RESERVED: AE-P0.1 test registration only` |

## Bus state protocol

Only the orchestrator changes ticket state. Valid transitions are:

```text
HOLD -> READY -> ASSIGNED -> ACTIVE -> READY_FOR_REVIEW
     -> CHANGES_REQUESTED -> READY_FOR_REVIEW -> APPROVED
     -> INTEGRATING -> INTEGRATED
```

`BLOCKED` may be entered from any active state. It returns to `READY` only after
the orchestrator records the resolved dependency or decision.

Bus subjects use `[ticket][state] summary`. Git SHAs and test artifact paths are
included in handoffs. The bus never substitutes for a commit, review, or gate.
Every assignment names the exact task-packet commit as well as the product base.
Amending a packet automatically returns its owner to `HOLD`; work resumes only
after the owner acknowledges the new packet SHA. Reviewer and implementer are
never evaluated against different packet generations.

## Evidence log

| Date | Event | Evidence |
|---|---|---|
| 2026-08-09 | Architecture audit plan created | `ARCHITECTURE_EXCELLENCE_PLAN.md` |
| 2026-08-09 | Four-worker pool joined `/tmp/dawagents` | join messages from all four handles |
| 2026-08-09 | HOLD onboarding sent to all workers | `[AE-BOOT][HOLD]` bus messages |
| 2026-08-09 | Claude workers acknowledged target path, plan, and HOLD | `[AE-BOOT][ACK]` from `claude-worker-1` and `claude-worker-2` |
| 2026-08-09 | `backend` re-registered under corrected channel protocol | `join.mjs` reported `runtime Codex`; stale watcher marker removed |
| 2026-08-09 | Undo completion trigger received | `[UNDO][DONE] main 5bef283` from `refactor` |
| 2026-08-09 | Architecture branch fast-forwarded | `d04331d..5bef283`, exact `main`/`origin/main` match |
| 2026-08-09 | Governance bootstrap committed | `762fe34` adds the approved plan and mutable execution ledger |
| 2026-08-09 | Frozen baseline independently acknowledged | `[AE-P0][ACK]` from all four workers; exact path/SHA/HOLD confirmed |
| 2026-08-09 | Existing worktrees inventoried read-only | At inventory time: `daw` clean at `5bef283`; `daw-play` detached/clean at `3259862`; frontend-owned `daw-web` dirty/unmerged at `97348ed`; no worktree modified |
| 2026-08-09 | Frontend worktree status updated by its owner | `daw-web` later clean at `afdb605`, containing `main` with divergence `0/220`; ownership and backend baseline unchanged |
| 2026-08-09 | Cross-worktree verification substitution reproduced | `tools/verify.sh` and `tools/webstack.sh` target sibling worktrees; other executable fallbacks target `/Users/jak/src/daw-web`; tracked `ui-web/node_modules` is an absolute symlink |
| 2026-08-09 | Clean RelWithDebInfo configure captured | CMake 4.3.3, Apple clang 17.0.0, arm64 Darwin, Boost 1.90.0, JUCE cache reports 8.0.12, SHM v37 |
| 2026-08-09 | Build-contract drift reproduced | CMake requests C++17 while source uses C++20 defaulted comparisons; unrestricted parallel build drove load average above 175 and was safely resumed with six jobs |
| 2026-08-09 | Clean baseline build completed | `cmake --build build-ae-baseline --parallel 6` completed all targets successfully |
| 2026-08-09 | Canonical hard-coded build root refreshed | `cmake -S . -B build ... && cmake --build build --parallel 6` completed successfully before CTest |
| 2026-08-09 | Rust baseline compiled and unit-tested | workspace build/no-run green; bridge+agent 36, sidecar 75, patcher 3 |
| 2026-08-09 | Agent e2e result withdrawn after resource audit | 34/34 tests reported pass, but `DAW_ENGINE_TEST_MODE=1` still opens the physical device; the 22.32-second run overlapped frontend's sweep and must be repeated under exclusive ownership |
| 2026-08-09 | Attributable baseline record opened | `docs/architecture/baselines/AE-P0-2026-08-09.md`; CTest, local web dependencies, and objective audio explicitly pending |
| 2026-08-09 | AE-P0.1 assigned and acknowledged | `/Users/jak/src/daw-ae-p0-roots` on `ae/p0-roots`; owner `codex-worker-1`, reviewer `claude-worker-1`; packet `docs/architecture/tasks/AE-P0.1.md` |
| 2026-08-09 | Symlink-install hazard stopped before execution | Reviewer proved `npm ci` would traverse the tracked link and rewrite frontend dependencies; packet amended to unlink only the verified symlink before local install |
| 2026-08-09 | AE-P0.2 read-only discovery packet prepared | `docs/architecture/tasks/AE-P0.2-discovery.md`; no implementation authorization |
| 2026-08-09 | AE-P0.2 discovery delivered for independent review | Report derives the 211-test partition, writable-resource inventory, RunContext/provenance proposal, nine negative controls, migration slices, and five unresolved ADR questions; `codex-worker-2` review active |
| 2026-08-09 | AE-P0.2 discovery independently rejected | Core counts reproduced, but provenance lacked artifact/source binding, RunContext had no coherent creator/lifecycle, endpoint allocation retained TOCTOU, Rust/web globals were incomplete, controls were mislabeled, and ownership overlapped; corrected report requested |
| 2026-08-09 | Cross-worktree runtime ownership respected | Full CTest deferred while frontend's 66-suite gate owns engine/audio resources; frontend will send an explicit clear signal |
| 2026-08-09 | Canonical operating brief corrected | `AGENTS.md` now states that test mode still opens audio, Codex must not use the Claude watcher, and SHM is v37 rather than v15 |
| 2026-08-09 | AE-P0.1 exposed a pre-existing web unit failure | 120/121 pass; command-caller audit ignores CLI and reports three CLI-reachable commands as unexplained. No scope expansion granted; `AE-P0.3` records the still-red gate |
| 2026-08-09 | Frontend released shared runtime resources | Owner proved no engine, host, sidecar, Node suite, or sweep remained; canonical backend baseline then ran exclusively |
| 2026-08-10 | Canonical CTest baseline completed | 204 passed, six failed, one skipped of 211 in 1,404.03 seconds; JUnit `build/ae-p0-ctest.xml` SHA-256 `2db255c070cce67c996bd4aaa7cb66b0494de52b5779610d9f9ed9f6f63bcc3d` |
| 2026-08-10 | CTest failure cluster classified without waiver | Governance freshness failed; five VST-backed checks had no plugin cache/parameters/state; `audio_stability` skipped with no loaded callbacks. Engine and host artifacts were newer than `event_payloads.h` and matched recorded hashes |
| 2026-08-10 | CTest mutated source-tree paths | `readback_check` left an untracked preset, which was recorded and removed; four checks appended eight events to ignored `presets/projects/history.jsonl`, which was preserved because no pre-run content hash existed |
| 2026-08-10 | Agent engine e2e rerun exclusively | 34/34 passed in 21.92 seconds with no competing suite; device-derived rate/block-size limitation remains explicit |
| 2026-08-10 | Normal-mode capture path exercised | Structurally valid all-zero 44.1-kHz WAV and expected-silence metric pass recorded; positive tap liveness is unproven, seven underruns make realtime stability red, and shutdown reported a control-header receive failure |
| 2026-08-10 | Shared runtime resources released back to frontend | Backend declared no remaining engine/host/sidecar suite; frontend resumed its 66-suite sweep |
| 2026-08-09 | AE-P0.2 V2.1 independently rejected | Default-substitution count corrected to 118; compiled/web resources, immutable lifecycle, IPC entropy, coherent artifact-record substitution, ownership, and audio serialization remained incomplete |
| 2026-08-09 | AE-P0.2 V3 independently rejected | Trusted source/build anchor, executed-artifact bijection, incremental generation semantics, closed schemas, endpoint lifecycle, crash-safe device lease, exact inventory/ownership, and adversarial controls remained incomplete; standalone V4 requested |
| 2026-08-09 | AE-P0.1 scope amended for credential isolation | Final red-team found own-stack sidecar CWD could discover parent/home `.env` and make an ambient paid call; `ui-web/test/stack.mjs` narrowly added with fail-closed regression requirements |
| 2026-08-10 | AE-P0.1 implementation handed off | `d722306` from parent `2f57427`; 12-path scope; independent exact-SHA review assigned before integration |
| 2026-08-10 | AE-P0.2 iteration stopped at the ADR threshold | Four independently rejected discovery designs were converted into an eight-decision dossier; the compiled-test inventory then closed all 48 targets / 66 linked source files |
| 2026-08-10 | AE-P0.2 umbrella ADR proposed | `docs/architecture/decisions/AE-P0.2-attributable-isolated-execution.md`; implementation remains blocked pending exact-SHA review, AE-P0.1 integration, and an ownership manifest |
| 2026-08-10 | Upstream product baseline advanced during preflight | `main` and `origin/main` moved from `5bef283` to `62bafdc` by 223 commits; the 62bafdc execution baseline is now selected, while historical evidence remains tied to 5bef283 |
| 2026-08-10 | Architecture implementation placed on coordination hold | All four workers were told to preserve work and make no edits, builds, tests, integration, merge, or rebase pending the owner's exact baseline choice |
| 2026-08-10 | AE-P0.1 exact-SHA review requested changes | Commit `d722306` remains clean and preserved; findings cover tracked bytecode/path forms, index-versus-live scanning, negative-control gaps, credential opt-in, legacy override rejection, and checkout-local dependency isolation |
| 2026-08-10 | AE-P0.1 review addendum found a launcher regression | The rewritten free-port probe assigns an expected `lsof` status 1 under `set -euo pipefail`, aborting `tools/webstack.sh` before normal startup; repair is frozen with the rest of the packet pending baseline selection |
| 2026-08-10 | Task-packet handoff invariant strengthened | AE-P0.1 was implemented from a branch predating later packet amendments; every future assignment and amendment must name the exact packet SHA and require owner acknowledgement before work resumes |
| 2026-08-10 | AE-P0.1 current-main repair packet committed | Worktree `/Users/jak/src/daw-ae-p0-roots-current`, branch `ae/p0-roots-current`, packet commit `016eea4`; owner assignment sent with exact baseline and packet SHA |
| 2026-08-10 | AE-P0.1 owner acknowledged exact packet | `codex-worker-1` confirmed `016eea4` and `62bafdc`; old `d722306`, main, and old repair worktree remain preserved |
| 2026-08-10 | AE-P0.1 packet amended before implementation | `ce09485` explicitly adds discovered live surfaces; prior `016eea4` is superseded and owner re-acknowledgement is required |
| 2026-08-10 | AE-P0.1 owner acknowledged amended packet | `codex-worker-1` confirmed `ce09485`; implementation may proceed only within the amended ownership table |
| 2026-08-10 | AE-P0.1 packet amended for credential and unique-log callers | `ace551b` adds explicit paid/demo credential opt-ins and validated segment log-locator ownership; owner acknowledgement required before resuming |
| 2026-08-10 | AE-P0.1 owner acknowledged packet `ace551b` | Exact current packet acknowledged; implementation resumed only within its amended ownership and invariant scope |
| 2026-08-10 | AE-P0.1 current-main packet amended and committed | `258f423` records credentialed demo callers and validated unique log-locator ownership in the correct current-main packet; implementation paused for exact-SHA handoff |
| 2026-08-10 | AE-P0.2 ADR independently approved | `codex-worker-2` approved exact SHA `7dff997`; no blockers remain in schema/trust, provenance, allocation, terminal selection, fixture closure, inventory, ownership, or GC controls |
| 2026-08-10 | Current-main metadata-only configure completed | Fresh `/Users/jak/src/daw/build-ae-current` generated from `62bafdc` with RelWithDebInfo and patcher Rust enabled; no build, test, install, or runtime process launched |
| 2026-08-10 | AE-P0.2 current-main delta checked read-only | Core findings survive at `62bafdc`; registered tests changed from 213 to 214 and shell inventory from 153 to 156, so configured counts and the complete inventory must be regenerated after baseline selection |
| 2026-08-10 | AE-P0.1 current-main implementation integrated | Product `/Users/jak/src/daw` now contains packet `258f423` and reviewed repair chain `2f7aa93 -> 88f4449 -> c3642a4 -> e4e08de -> 0f48f69 -> e19128e -> 3b53a29`, ending at integration commit `71758c0`; Claude approved exact `3b53a29` and verified helper/checker canaries plus sidecar readiness |
| 2026-08-10 | AE-P0.1 post-integration verification | Product repository integrity self-test and production scan pass: 730 tracked/worktree paths, 670 index/worktree live files, zero non-ignored untracked paths; READY-retirement self-test, shell syntax, and Node syntax pass |
| 2026-08-10 | AE-P0.1 nonblocking follow-ups recorded | F1: replace five source-text controls with behavioral hostile probes; F2: remove/initialize redundant post-wait listener diagnostic recheck; F3: reconcile wrapper-only policy with repository_root canonicalization prose and document `/tmp` behavior |
| 2026-08-10 | AE-P0.2 Lane 0 assigned | `codex-worker-2` asked for a pre-enumerated packet and isolated proposal before adding generated OwnershipManifest/OwnershipTransfer schemas; no downstream lane authorized |
| 2026-08-10 | AE-P0.2 Lane 0 packet approved and implementation authorized | Corrected packet `6287ffd` independently approved by `claude-worker-2`; 25 additive paths, 755-path reference set, TS compiler deferred to toolchain-owned lane; Lane 0 implementation now active |
| 2026-08-10 | AE-P0.2 Lane 0 corrective review requested | Second independent review identified invalid JSON-domain handling and divergent digest preimages; corrective implementation `89d0f6cfddb6350c15d239f13058221d5e1b6051` was prepared for exact review |
| 2026-08-10 | AE-P0.2 Lane 0 corrective packet superseded with final path authority | Corrective packet `e52804d31075443d03fb63e210176bd41efd13a3` supersedes prior drafts; it enumerates 25 implementation paths, excludes the governance packet from the manifest, pins the 755-path reference set, and adds baseline/dotfile controls; implementation remains paused pending exact owner/reviewer acknowledgement |
| 2026-08-10 | AE-P0.2 Lane 0 corrective completed | Baseline amendment packet `8f359009f808d84cdc0aa9fcb4afbd675b7d5f99` approved by owner and independent reviewer; candidate `ddb05bd9803c1b30ba2b730d7dc4b8680a95285c` based on Undo-complete `7710401d72029482c8f3d15869d58dce7e246def`; focused gates 15/15, independent schema/bundle/anchor/validator identities, C++17/Rust2021 fixtures, sabotage controls, and exact 756-path reconciliation all passed; integrated to product as `75c6f06` |
| 2026-08-10 | AE-P0.2 manifest authority follow-up recorded | P1.1 owner reported an independently derived 755-path authority interpretation that omits the ratified amended-base delta `tools/gesture_drag_check.sh`; direct `validate-cli.mjs` at product `75c6f06` includes that delta and returns PASS with 756 paths. Keep this as an explicit follow-up: all authority consumers must derive the same ratified 756-path set, with no baseline interpretation left implicit. It does not block documentation-only P1.1. |
| 2026-08-10 | AE-P0.3 packet amended after isolated baseline reproduction | Packet `dab40fbd1f4ed174bb7f0634a6df488c0eb98731` records isolated 141/146 with five plugin-cache fixture failures, caller target passing, and focused-control acceptance; owner `codex-worker-2` acknowledged exact SHA |
| 2026-08-10 | AE-P0.3 implemented, independently approved, and integrated | Claude approved exact `a265a7b7b338909b9b8d4a08796d89b94dacad98`; product `/Users/jak/src/daw` integrated it as `d7fc58c`; targeted caller audit and five controls pass; documented plugin-cache fixture failures remain environmental |

## AE-P0 baseline findings

These are baseline observations, not accepted debt and not evidence from a
sibling checkout:

- Canonical source is `/Users/jak/src/daw-backend` on `architecture-audit` at
  `5bef283798b59c2c4f5720292554c7ab8c265be6`.
- `/Users/jak/src/daw-web` was divergent and unmerged during the initial inventory
  and was later reported clean at `afdb605` by its owner. It remains a divergent,
  frontend-owned worktree. The architecture program will neither test it as the
  backend baseline nor modify it.
- `tools/verify.sh`, `tools/webstack.sh`, `tools/ask_path_check.sh`,
  `tools/demo_rehearsal.sh`, and `ui-web/test/e2e.mjs` contain executable
  sibling-worktree fallbacks. `ui-web/node_modules` is a tracked absolute
  symlink. A dedicated `AE-P0.1` ticket must replace these with self-locating,
  fail-closed behavior and add a repository guard.
- The root CMake configure derives JUCE from `$HOME/src/juce/JUCE`; that source
  tree has no queryable Git provenance. Dependency provenance is therefore
  incomplete even though CMake identifies JUCE 8.0.12.
- The clean baseline build is intentionally bounded to six jobs after the
  unrestricted build oversubscribed the machine. The interrupted build changed
  only disposable build artifacts and resumed in the same isolated directory.
- Read-only AE-P0.2 discovery found that a bare noncanonical `ctest --test-dir`
  sends 118 direct shell engine checks to canonical `build/`: 103 are
  override-immune and 15 are override-aware but default there. Its V2.1 and V3
  reports were independently rejected for deeper contract gaps. The canonical
  C++ and Rust debug artifacts were rebuilt before the full sweep.
- The canonical CTest result is honestly red: 204 pass, six fail, one skip. Five
  failures form a missing-plugin-cache/fixture cluster; the suite must become
  hermetic and fail-fast rather than relying on machine-local plugin state.
- The objective capture created and metrically accepted an all-zero WAV. That
  passes the expected-silence content assertion but does not prove positive tap
  liveness; seven underruns separately leave realtime stability red.
