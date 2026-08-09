# Architecture Excellence Remediation Plan

Status: **ACTIVE -- AE-P0 baseline verification in progress**

Worktree: `/Users/jak/src/daw-backend`

Frozen upstream baseline: `5bef283798b59c2c4f5720292554c7ab8c265be6`

Prepared: 2026-08-09

This plan converts the architecture audit into an implementation program. Its
two entry conditions were satisfied on 2026-08-09:

1. The Undo refactor completed and landed on `main` at the frozen baseline above.
2. The owner explicitly authorized execution after the `refactor` completion
   handoff.

Phase 1 remains blocked until the complete AE-P0 build/provenance gate passes.

## Objective

Deliver a release-grade, deterministic musical virtual machine whose real-time
behavior, cross-process contracts, project persistence, browser boundary, and
build provenance are demonstrably correct.

"Excellent architecture" means the following properties are enforced by the
design and executable gates, rather than depending on comments or contributor
discipline:

- One authoritative document model and one generated wire contract.
- Deterministic command arbitration and exactly attributable command results.
- Formally specified cross-process publication and crash behavior.
- No locks, allocations, unbounded work, or deadline-scale socket waits on
  hard real-time paths.
- Every audio source traverses one compiled track/routing graph.
- Transactional project and plugin-state persistence.
- Authenticated, typed, bounded browser communication.
- Hermetic builds whose reports identify the exact source SHA they exercised.
- Unsupported behavior is rejected explicitly, never accepted and ignored.

## Program guardrails

- Correctness and security take priority over feature work.
- Reproduce a defect with an executable test or litmus before changing it.
- Change one architectural invariant per ticket.
- No compatibility hacks inside the real-time path. Compatibility is an
  explicit versioned adapter with a deletion ticket.
- One writer owns each merge-hotspot file at a time.
- The task owner may not be the sole reviewer of concurrency, unsafe Rust,
  security, persistence, or real-time changes.
- An agent stops and reports `BLOCKED` when the requested solution requires a
  new architectural decision, overlaps another task's ownership, or expands
  beyond its written scope.
- The bus is a notification/control plane. Git commits, this plan, task records,
  ADRs, and test artifacts are the durable source of truth.

## Orchestration layout

### Initial agent pool

Use one orchestrator plus four workers:

- Orchestrator: this agent in `/Users/jak/src/daw-backend`.
- Two Codex workers.
- Two Claude workers.

Run at most two implementation writers concurrently during the safety and
contract phases. Pair them across models:

- Lane A: Codex implementation owner, Claude independent reviewer.
- Lane B: Claude implementation owner, Codex independent reviewer.

Swap those roles between tickets. This reduces correlated blind spots and keeps
both models practiced at implementation and adversarial review. After the
schema, command, and module ownership boundaries are frozen, one additional
Claude worker and one additional Codex worker may be added, for at most three
concurrent implementation lanes. Do not exceed three writers without an
explicit dependency/file-overlap review.

### Orchestrator responsibilities

The orchestrator does not merely distribute prompts. It owns:

- The dependency graph and `HOLD`/`READY`/`ACTIVE` state of every ticket.
- The approved baseline SHA and worktree provenance.
- File ownership and merge-hotspot locks.
- Task packet quality and acceptance criteria.
- Reviewer assignment, with a different model where possible.
- Integration order and conflict resolution.
- Full-suite and cross-workstream gates.
- Updating this plan, ADRs, and `AGENTS.md` when operational knowledge changes.
- Stopping the program when evidence contradicts an architectural assumption.

The orchestrator may make small integration-only edits, but should not become a
third feature writer while two worker lanes are active.

### Worktree policy

Each implementation ticket receives its own Git worktree and branch from the
approved `/Users/jak/src/daw-backend` baseline. Suggested naming:

```text
/Users/jak/src/daw-ae-p1-schema       branch ae/p1-schema
/Users/jak/src/daw-ae-p1-session      branch ae/p1-session
/Users/jak/src/daw-ae-p2-ring         branch ae/p2-ring
```

Each worktree owns independent:

- CMake build directory.
- Cargo target directory.
- `node_modules` installation.
- Ports, SHM names, project scratch directories, logs, and capture files.

Agents never share a writable checkout. They commit their own changes; only the
orchestrator integrates reviewed commits into the architecture branch.

### `/tmp/dawagents` protocol

Use `/tmp/dawagents` for cross-harness communication between Claude, Codex, and
the orchestrator. No second orchestration service is needed initially.

Every participant first registers its real handle and channel root:

```text
node /tmp/dawagents/hook/join.mjs /tmp/dawagents <handle>
```

The printed runtime must match the actual harness. A Codex session that prints
`runtime Claude Code`, or the reverse, stops and reports the contaminated
environment rather than registering under a false delivery contract.

Every participant gets a stable, ticket-specific handle, for example:

```text
backend                         orchestrator
ae-p1-schema-codex              implementation owner
ae-p1-schema-review-claude      independent reviewer
ae-p1-session-claude            implementation owner
ae-p1-session-review-codex      independent reviewer
```

Every turn begins with `poll.mjs <handle>`. Delivery after that is
runtime-specific:

- **Codex:** never runs `watch-next.mjs`. The registered Stop guard refuses to
  end a turn while mail is unread. Mail arriving while Codex is idle waits for
  its next turn, so an agent expecting a reply polls again before ending.
- **Claude Code:** follows the delivery instructions printed by `join.mjs`; its
  harness may use `watch-next.mjs` because background-task completion can resume
  that runtime.

Messages use short subjects and an inline body or `--body-file`; code and large
diffs stay in Git. Channel registration is not accepted on faith: each new
session receives a test message and must demonstrate that its delivery guard is
live before it can own a ticket.

Subject convention:

```text
[AE-P1.1][ASSIGN] Generate v37 schema
[AE-P1.1][ACK] Scope and baseline confirmed
[AE-P1.1][BLOCKED] Layout decision required
[AE-P1.1][READY_FOR_REVIEW] Commit <sha>
[AE-P1.1][REVIEW] Changes requested
[AE-P1.1][APPROVED] Evidence accepted
[AE-P1.1][INTEGRATED] Architecture branch <sha>
```

The bus is append-only notification. A task is not complete because a message
says so; it is complete only when its reviewed commit and evidence satisfy the
ticket's gate.

### Task packet

Every assignment must contain all of the following:

```text
Task ID and title
Outcome: observable result, not an activity
Baseline: canonical SHA and worktree path
Dependencies: integrated ticket IDs
Owned files/directories
Read-only files allowed for context
Invariants that must remain true
Required reproducer or failing test
Implementation constraints and non-goals
Acceptance commands and expected observations
Negative controls/fault cases
Evidence to return
Reviewer and review focus
Stop/escalation conditions
```

Bad goal: "fix the ring."

Good goal: "A Rust consumer can never observe an unpublished entry; every entry
is reclaimed exactly once; producer pause/crash cannot corrupt another client;
the cross-process wraparound, SIGSTOP, and crash-phase tests pass on ARM64."

### Closed feedback loop

Each ticket follows the same loop:

1. **Assign:** orchestrator sends the complete task packet and reserves file
   ownership.
2. **Acknowledge:** worker confirms baseline, scope, invariants, and test command
   before editing.
3. **Reproduce:** worker adds or identifies a failing test/negative control.
4. **Implement:** worker makes the smallest coherent architectural change.
5. **Verify locally:** targeted tests, relevant suite, warning/static checks,
   and worktree cleanliness.
6. **Handoff:** worker commits and reports SHA, changed invariants, commands,
   results, known limitations, and risks.
7. **Adversarial review:** independent reviewer reads the task packet and diff,
   constructs counterexamples, runs tests, and reports findings without silently
   expanding scope.
8. **Repair:** original owner addresses findings. Review repeats until accepted
   or escalated.
9. **Integrate:** orchestrator rebases/cherry-picks in dependency order and runs
   the integration gate from the architecture worktree.
10. **Close:** orchestrator records evidence, releases file ownership, deletes
    temporary compatibility paths when their deletion ticket is reached, and
    marks the next dependent task `READY`.

If the same architectural uncertainty survives two review cycles, stop the
ticket and write an ADR/request a decision instead of iterating blindly.

## Entry gate: Undo refactor and baseline freeze

ID: `AE-P0`

No remediation work begins before this gate.

Work:

- Wait for the Undo refactor to finish.
- Integrate or intentionally park its commits.
- Confirm `/Users/jak/src/daw-backend` is clean.
- Record the exact baseline SHA and branch.
- Inventory divergence between active worktrees without modifying them.
- Ensure commands launched from this repository build and test this repository,
  not `/Users/jak/src/daw-web` or another sibling checkout.
- Remove implicit cross-worktree source/build lookup in a dedicated reviewed
  ticket after approval.
- Give every future worktree isolated build/test resources.
- Capture known baseline results for CMake build, CTest, Rust workspace and agent
  e2e, patcher tests, web unit/goldens/allocations, and objective audio tests.
- Add build provenance output: source realpath, Git SHA, dirty state, compiler and
  tool versions, preset, protocol version, and important dependency revisions.

Gate:

- Owner has said `GO` after Undo completion.
- One canonical baseline path, branch, and SHA are written here.
- All active agents acknowledge that baseline.
- A repository check rejects implicit `/Users/jak`, `daw-web`, and absolute
  tracked symlink references.
- Two worktrees can build/test concurrently without sharing artifacts, ports,
  SHM, project directories, or logs.
- Baseline failures are documented rather than silently skipped or blessed.

### AE-P0.1: Self-contained repository and verification roots

- Replace every executable hard-coded checkout path with a root derived from the
  running script/module or an explicit, validated override.
- Remove the tracked absolute `ui-web/node_modules` symlink; dependency installs
  are local, ignored artifacts in each worktree.
- Add a fail-closed repository guard for tracked absolute symlinks and
  user-specific/sibling-worktree paths in executable build and verification
  surfaces. Documentation examples are classified explicitly rather than hidden
  by a broad exclusion.
- Make primary verification entry points print and validate the source realpath,
  Git SHA, dirty state, build realpath, and relevant artifact provenance before
  running a suite.

Gate: launching verification from an arbitrary working directory exercises the
checkout containing the script. A deliberately poisoned sibling checkout is
never read or executed. The repository guard fails on synthetic absolute-path
and absolute-symlink negative controls and passes the real tree.

### AE-P0.2: Isolated worktree run context and provenance

- Define one run-context contract for build directories, Cargo targets, ports,
  SHM names, project scratch space, logs, captures, and temporary files.
- Derive collision-resistant per-run resource identities and pass them through
  every primary CMake/CTest/Rust/web entry point; a PID or fixed default alone is
  not an isolation boundary.
- Emit a machine-readable manifest containing source/build realpaths, Git SHA and
  dirty state, compiler/tool versions, preset/configuration, protocol version,
  dependency identities, test selection, and artifact hashes where relevant.
- Require JUCE and other non-repository dependencies to have an explicit resolved
  path and reproducible identity; an unversioned `$HOME` fallback is not accepted
  provenance.

Gate: two clean worktrees configure, build, and run representative engine, Rust,
web, and SHM tests concurrently with no shared writable path or resource name.
Each result is attributable to its own manifest, and deliberately swapping an
artifact or source root fails before tests begin.

### AE-P0.3: Truthful command-caller coverage

- Correct the command-caller audit so its claimed population and inspected
  surfaces agree. A command reached only through `daw-cli` is a caller, not an
  unexplained engine command.
- Keep the audit non-vacuous for sidecar, agent, and CLI sources independently;
  a missing/unparsed surface must fail rather than silently shrink coverage.
- Preserve the reverse stale-reason check: once a command gains a caller, any
  recorded unused reason must be removed.
- Treat source scanning as an interim guard. The generated protocol/operation
  registry in Phase 1/2 must eventually replace hand-parsed command lists.

Gate: the web unit suite is green; a synthetic CLI-only caller is accepted; a
synthetic truly uncalled command is rejected; and removing any inspected caller
surface makes the audit fail. This ticket changes audit truthfulness only, not
engine/client behavior or wire definitions.

## Phase 1: Contracts and safety foundations

Phase 1 is mostly serial at merge hotspots. Prototype/test work may occur in new
files in parallel, but only one integration owner edits `shared_memory.h`,
`event_payloads.h`, `layout.rs`, `control.rs`, or protocol versions.

### AE-P1.1: Protocol and memory-model ADR

Specify before changing layout:

- Supported operating systems, architectures, endianness, atomic widths,
  alignment, and lock-free assumptions.
- Ownership and lifecycle of every ring, lane, snapshot, request slot, and mmap.
- Crash, SIGSTOP/debugger pause, reconnect, restart, and backpressure semantics.
- Command identity, ordering, fairness, idempotent retry, and batch semantics.
- Snapshot reliability versus state invalidation and result reliability.
- Major/minor version and capability negotiation.

Gate: every rule has an executable invariant or named test; no unresolved choice
remains about lane reclamation, result delivery, or snapshot publication.

### AE-P1.2: One generated wire schema reproducing v37

- Introduce one editable schema that generates C++ PODs/enums/constants, Rust
  `repr(C)` types, discriminant/offset/size/alignment assertions, a layout
  manifest/fingerprint, and `SHM_LAYOUT.md`.
- Reproduce current v37 byte-for-byte before adding fields.
- Surface and resolve current enum/document drift.
- Keep patcher ABI in the same generation system or a clearly separate generated
  schema with exhaustive guards.
- Generated files are never edited by hand; regeneration must be byte-identical
  in CI.

Gate: C++ and Rust compile against the generated v37 manifest, no production wire
definition remains hand-maintained, and generated documentation is current.

### AE-P1.3: Validated shared-memory mapping

- Parse the header into an untrusted descriptor first.
- Check mapping length, checked offset/length arithmetic, alignment, stride,
  capacity limits, non-overlap, and protocol fingerprint.
- Expose typed regions only after complete validation.
- Replace scattered raw pointer arithmetic with validated views.

Gate: malformed/truncated/corrupt-header fuzz and property tests cannot construct
an out-of-bounds or misaligned typed view.

### AE-P1.4: Atomic render snapshot publication

- Introduce one API for publishing/loading immutable track render snapshots.
- Remove every plain live assignment to the shared snapshot pointer.
- Cover patcher edits, track tombstone/reuse, aux reconciliation, project load,
  and playback concurrency.

Gate: concurrency hammer and TSan-supported tests show no mixed atomic/plain
access or lifetime violation.

### AE-P1.5: HostSession lifecycle and watchdog

- Create a `HostSession` state machine owning controller, mapping generation,
  readiness, completion progress, watchdog, restart, and shutdown.
- Monitor health independently of producer backpressure.
- Validate all message sizes before allocation and all mmap fields before use.
- Dispatch legal zero-body messages, including shutdown.
- No mapping, watchdog, or reply from an old generation may affect a new host.

Gate: frozen and killed host fixtures recover within a bounded deadline while
transport and unaffected tracks continue; graceful shutdown reaches its handler.

### AE-P1.6: Browser threat model and immediate security boundary

- Sidecar serves static UI and one authenticated duplex WebSocket on one origin.
- Use a high-entropy per-launch capability and exact Origin validation.
- Authenticate before SHM attach, client accounting, agent setup, or state send.
- Bound connection count, threads/tasks, frames, queues, command rate, and paid
  agent calls.
- Keep viewport, acknowledgements, and health per session.
- Replace structural string parsing/formatting with typed Serde envelopes.
- Add a restrictive CSP and fail closed when the secure command surface is not
  available.

Gate: hostile-origin, wrong/replayed-token, oversized-frame, connection-flood,
slow-client, reconnect, and lifecycle tests pass with bounded resources.

## Phase 2: Reliable control plane and state publication

Phase 2 changes the control-plane contract. It should be a deliberate monorepo
cutover, not a long-lived dual protocol inside one SHM segment.

### AE-P2.1: Producer-owned command and result lanes

- Keep RT audio/event SPSC rings separate from UI control transport.
- Give each client a producer-owned SPSC command lane and reliable result lane.
- A producer writes a complete entry before release-publishing its write position.
- Remove reserve-before-fill, `ready`, two-second abandoned-slot recovery, and
  bystander peeking.
- Reclaim a client lane only after graceful close/ack or confirmed process death;
  a paused live process remains paused.
- Drain lanes with deterministic bounded round-robin arbitration.

Gate: multi-process tests run through many wraps with slow, killed, and SIGSTOP'd
producers without loss, duplication, tearing, starvation, or false reclamation.

### AE-P2.2: Correlated transactional command results

Define commands and results around:

```text
Command: client_id, command_id, scope, base_version, opcode, payload
Result:  client_id, command_id, status, reason, resulting_version, returned_ids
```

- Produce exactly one attributable terminal result per command.
- Deduplicate retries and cache a bounded result window.
- Reserve result capacity before applying a mutation.
- Publish authoritative state before the success result.
- Make note/chord batches all-or-none and return created stable IDs.
- Replace version movement, ring peeking, journal inference, and timeout silence as
  acknowledgement mechanisms.
- Reject unknown tracks/devices and every unsupported command state explicitly.

Gate: two clients interleave same- and different-scope edits; every command gets
exactly one matching result, unrelated version changes satisfy nothing, stale
bases leave state unchanged, and retries are idempotent.

### AE-P2.3: Transactional bulk transport

- Bind large payloads to client and command identity.
- Carry exact byte length, checksum, and commit/abort state.
- A partial upload can never dispatch or merge with a later message.
- Replace repeating 16-bit IDs; use a durable monotonic or random 128-bit identity.

Gate: collision, partial enqueue, disconnect, retry, checksum failure, and memory
limit tests produce explicit outcomes and no hybrid payload.

### AE-P2.4: One snapshot publication abstraction

- Provide one reviewed C++ writer/Rust reader helper.
- Use small seqlocks only where their body-copy race model is explicitly valid.
- Prefer immutable double/triple buffers for large clip/source, chain, routing,
  modulation, automation, parameter, waveform, and meter state.
- Replace every end-only version check and ad-hoc fence.
- State invalidations are gap-detectable and coalescible; reliable command results
  remain separate.
- Multi-entry snapshots include ID, item count, and checksum and publish only when
  complete.

Gate: ARM64 stress pauses writers at every field range without any accepted mixed
frame; a dropped invalidation always converges through a complete snapshot.

### AE-P2.5: Migrate clients

Migration order:

1. Sidecar.
2. Agent.
3. CLI.

Bridge API becomes `submit(command) -> ticket` and `await_result(ticket)`. Remove
`wait_for_*version`, unsafe diff peeks, and "unknown means applied." Observations
must use stable IDs and coherent snapshots, not slot indices or tombstones.

Gate: sidecar, agent, and CLI pass the same multi-author outcome matrix.

## Phase 3: Executable audio architecture

Phase 3 begins only after command, snapshot, and HostSession boundaries are
stable. Two main implementation lanes may operate in parallel while shared
graph types have one owner.

### AE-P3.1: Compile an immutable RenderPlan and RoutingGraph

- Separate `EngineDocument` from immutable render state.
- Compile typed audio and MIDI edges, bus widths, mixer membership, PDC, route
  latency, and explicit feedback delays.
- Reject invalid, cyclic, or unsupported routing states transactionally.
- Make execution and latency independent of track iteration order.
- Implement or reject every persisted routing field; never keep decorative state.

Gate: route output is identical when source/destination IDs or scheduling order
are reversed, and documented `Master`, `Track`, and `None` semantics are exact.

### AE-P3.2: Make every audio source a track source node

- Audio clips, samplers, routed input, generators, and plugin output enter the
  same pre-chain track bus.
- Remove direct audio-region-to-master mixing.
- Model clip loop period, source time, anchored tempo conversion, fades, and warp
  explicitly.
- Apply mute, solo, devices, routing, metering, PDC, offline rendering, and capture
  uniformly.

Gate: audio clips obey all track semantics, loop/fade correctly across tempo
changes, and render identically live and offline.

### AE-P3.3: Correct host preparation and VST3 processing

- Negotiate `{sample_rate, max_block_size, bus_layout}` before instantiation.
- Split host control/lifecycle work from the render loop.
- Implement proper VST3 bus arrays and `ProcessContext`.
- Preallocate event and parameter storage.
- Preserve velocity-zero note-off semantics.
- Verify microtonal tuning with tempo sync, sidechain, and multi-output together.

Gate: initial launch and restart pass at 44.1/48/96 kHz and 64/512/1024 frames;
combined tuned/tempo/sidechain/multi-output tests pass.

### AE-P3.4: Real sample-accurate automation

- Represent automation as bounded curves/segments rather than per-sample IPC
  messages.
- Deliver VST3 parameter changes at sample offsets or use deterministic sub-blocks.
- Musical events cannot be displaced by parameter traffic.

Gate: exact transition-sample tests pass for multiple concurrent ramps without
ring drops or missing notes.

### AE-P3.5: Real-time hardening

- Introduce preallocated per-thread/per-track block workspaces.
- Remove critical-path vector growth, blocking mutexes, environment lookup,
  unbounded sorts, and deadline-scale socket polling.
- Replace latest-block latches with block-ID-aware bounded queues.
- Instrument maximum/p99 producer, host, and callback times rather than averages.

Gate: allocation/lock traps report zero forbidden operations on hard real-time
paths, stress runs meet agreed p99/max deadlines, and no dropout is hidden by
repeated stale blocks.

## Phase 4: Authoritative state and transactional persistence

### AE-P4.1: One mutation gateway

- Every mutating opcode declares scope, expected revision, validation, mutation,
  version increment, publication, result, and undo behavior.
- Chain, routing, modulation, patcher, automation, sampler, clip metadata, and
  marker operations use the same transaction discipline.
- Unknown entity and unsupported-state outcomes are explicit.

Gate: compile-time command coverage and adversarial stale/unknown-entity tests
cover every mutating opcode.

### AE-P4.2: Engine-owned device parameter state

- Manual UI, automation, preset, and plugin-editor changes update one canonical
  per-device parameter/state model.
- Host state is a projection with acknowledgement and feedback, not authority.
- Crash recovery, undo/redo, and save/load use the same canonical state.

Gate: parameter changes survive host crash/restart and project save/load, are
undoable where specified, and never depend on which surface authored them.

### AE-P4.3: Atomic project generation

- Stage document, manifest, plugin blobs, and module assets together.
- Bind state to durable device identity plus plugin class UID/version/hash and
  exact resolved host mapping.
- Validate all writes and fsync/atomically swap the complete generation.
- Garbage-collect only from the committed manifest.

Gate: fault injection at every write/rename leaves either the old or new complete
project; missing/reordered plugins never receive another plugin's state; stale
sidecars never enter module output.

## Phase 5: Decomposition, build discipline, and portability

### AE-P5.1: Split ownership boundaries

Extract cohesive components after behavior is protected by typed contracts:

- `TrackDocument`
- immutable `TrackRenderState`
- `HostSession`
- compiled `RoutingGraph`
- `AudioSourceService`
- command transaction coordinator
- sidecar transport/session, protocol, broker, projection, project, and agent
  modules
- frontend bootstrap, generated protocol, state store, commands, and surfaces

Composition roots construct dependencies but contain no domain logic. Remove
legacy paths in the same ticket that replaces their final caller.

### AE-P5.2: Hermetic build and verification

- Add CMake presets for development, RelWithDebInfo, release, ASan, and TSan.
- Make engine targets build their required host, Rust patcher, bridge/CLI, and
  generated contracts.
- Pin JUCE, Boost policy, Rust, Node, and lockfile use.
- Resolve runtime helpers relative to the executable/install bundle, not cwd.
- Add compiler-aware warnings, static checks, and a single root verification
  entry point.
- Start with clean macOS/ARM64 CI. Add Windows only after platform process, SHM,
  and IPC abstractions exist and are tested.

Gate: a fresh checkout and one documented preset reproduce all artifacts; tests
cannot use stale binaries or sibling worktrees; release/performance tests prove
their build mode.

### AE-P5.3: Remove contract and documentation drift

- Generate volatile layout/version/opcode/capability tables.
- Keep prose for invariants and decisions, not copied offsets and numbers.
- Add freshness checks that fail on generated diffs.
- Remove obsolete GPUI and historical milestone instructions from current
  operational sections while preserving history where useful.

## Final release gate

The program is complete only when all of the following are true from a clean
approved worktree:

- CMake configure/build and the complete CTest inventory pass.
- Rust workspace, bridge, agent, CLI, sidecar, and engine e2e pass.
- Web unit, type, lint, capability, visual, and allocation gates pass.
- Generated schema and documentation are byte-identical after regeneration.
- ASan/UBSan and meaningful TSan/concurrency suites pass.
- ARM64 publication, producer crash/pause, restart, slow-client, corrupt-SHM,
  hostile-browser, and persistence fault matrices pass.
- Objective live/offline audio tests cover routing, audio clips, automation,
  microtonal, tempo-sync, sidechain, multi-output, PDC, and host restart.
- Performance evidence records max/p99 timing, queue high-water marks, dropouts,
  and allocation/lock violations at agreed 32- and 64-track scenarios.
- Two independent clients can edit concurrently with exactly attributable
  results and deterministic replay hashes.
- Windows is either implemented and gated or removed from current support claims.
- No temporary adapter, timer-based slot reclamation, unsafe diff peek, inferred
  command success, manual structural JSON parser, direct audio-to-master path, or
  obsolete protocol mirror remains.
- `git status` is clean after verification.

Required independent sign-offs:

- Concurrency/unsafe reviewer.
- Security reviewer.
- VST3/real-time audio reviewer.
- Persistence/backward-compatibility reviewer.
- Build/release reviewer using a fresh worktree.

## Program state

```text
AE-P0    ACTIVE -- baseline build/provenance verification
AE-P1.*  BLOCKED by AE-P0
AE-P2.*  BLOCKED by AE-P1
AE-P3.*  BLOCKED by AE-P1/P2 HostSession and contract stability
AE-P4.*  BLOCKED by AE-P2 transaction semantics
AE-P5.*  BLOCKED until replacement behavior is protected by gates
```

No implementation is authorized by this document alone.
