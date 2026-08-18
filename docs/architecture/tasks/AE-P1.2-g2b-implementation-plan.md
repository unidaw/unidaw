# AE-P1.2 G2-B — combined item-15/item-18 implementation plan

> Generated from `AE-P1.2-g2b-implementation-steps.json` by `tools/architecture/ae_p1_2_g2b_impl_steps_check.py --write`; do not edit by hand.

- Item 15 packet: `8ee5b3cdd34ef6c5538fac19074b4f442c0a8514`, manifest SHA-256 `a9583a4cb8a8fd45d1dc2ccfb683cf06f8758c55407bfdf6f3d5a424eea4465f`.
- Item 18 packet: `34f0d7b3abe6918a3578b0c5852ee22476bd8a75`, manifest SHA-256 `4fcd463c3ca68c63f7100ae13874fe5620920b2ed76d0f1d4b905da1ec6a9a41`.
- Frozen product base: `92dfdfe23cc7ff93f2ce14894a35d089e3d9e2b8` (tree `238ac970b5d61fe16055ede4c43a2978ddb11da7`).
- Implementation branch: `ae/p1-2-g2b-implementation` (this repository).

## This is one atomic change

The frozen contract is ONE change, not a sequence of independently shippable ones. Its records reference each other's constructs across the declared dependency graph in BOTH directions (R-DISPATCH-TICKET's ticket tuple names the sessionBlockTicket that R-TRANSACTIONAL-EVENT-BATCH introduces, while R-TRANSACTIONAL-EVENT-BATCH declares R-DISPATCH-TICKET as its dependency), so no partition of the records makes every step a state in which the whole contract is true. The steps below are therefore a BUILD ORDER for one atomic change, and every intermediate commit is a development state of that change. The branch is complete, and only then mergeable, when the completion_gate below is met.

## Steps

### Step 1 — Durable identity foundation and the protocol marker

Project-global stable device ids with a 0x7FFF ceiling and checked narrowing at every carrier; the live watermark and its two meeting points with the document (captureDocument stamps, applyDocument adopts); schema 6 with next_device_id; the schema 1-5 {trackId,oldDeviceId}->globalId migration map and its retained LegacyArtifactKeys; validation of an already-global document. Also bumps kControlVersion 14->15 and kShmVersion 41->42 in C++, Rust and SHM_LAYOUT.md.

R-PROTOCOL-VERSION fixes the VALUES (15 and 42) and states the reason as meaning changing under unmoved bytes. This step is the first that changes a meaning carried in SHM: UiPatcherNode.ownerDeviceId, packSamplerAddr's low half and kUiPatcherDeviceIdMask all stop being track-scoped. Bumping here rather than at the replay step means no commit on this branch ever ships changed SHM meaning under an unchanged version marker. The remaining meaning change the record names, the ReplayComplete payload gate, lands at step 6 under the same already-advanced marker.

Records closed (0): none. This step is PREPARATORY — it builds code a later step needs in order to close its records intact.

### Step 2 — Schema-6 document completion

Tagged automation targets (All / StableDevice / DisabledLegacyCompact with its reason and original compact number); the immutable artifact generation, sorted per-entry digest inventory, the four-row presence matrix, the save commit order, schema-6 load, and module packaging from the verified inventory only. After this step a schema-6 document carries every field R-DEVICE-ID-LIFETIME and R-PROJECT-TARGET-MIGRATION require of it.

Records closed (0): none. This step is PREPARATORY — it builds code a later step needs in order to close its records intact.

### Step 3 — Routing normalization and the 20-row matrix

The normalized directed routing graph, the exhaustive lane-by-kind matrix and its conflict rules, one-block-per-edge delivery, ascending {sourceTrackId,sourceBus,channel} fan-in, preFaderSend canonicalization, aux-child derivation, and the latency plan. Compiled INTO the snapshot revision at step 4.

Records closed (0): none. This step is PREPARATORY — it builds code a later step needs in order to close its records intact.

### Step 4 — The session ExecutionSnapshot

One immutable session ExecutionSnapshot as the sole execution authority, its monotonic revision, the candidate-compile-validate-publish transaction under the command-thread writer lock, and removal of TrackStateSnapshot.chainDevices, TrackStateSnapshot.routing and routesToMaster as execution authorities. Installs the P-EXECUTION-AUTHORITY-CONSUMERS census and its scanner.

Records closed (8): `P-EXECUTION-AUTHORITY-CONSUMERS`, `P-HOST-PLAN-MUTATIONS`, `P-SNAPSHOT-PUBLISHERS`, `R-DEVICE-ID-LIFETIME`, `R-HOST-PLAN-AUTHORITY`, `R-PROJECT-TARGET-MIGRATION`, `R-ROUTING-AUTHORITY`, `R-STABLE-DEVICE-TARGETS`

Tests landed (12): `T-ARTIFACT-PRESENCE-MATRIX`, `T-ARTIFACT-PROVENANCE`, `T-DEVICE-ID-LIFETIME`, `T-GLOBAL-DEVICE-IDENTITY`, `T-LEGACY-DISABLED-ROUNDTRIP`, `T-PLAN-RACE`, `T-PROJECT-TARGET-MIGRATION`, `T-ROUTING-ATOMICITY`, `T-ROUTING-BLOCK-DETERMINISM`, `T-ROUTING-MATRIX`, `T-STABLE-DEVICE-TARGETS`, `T-STATE-ARTIFACT-MIGRATION`

### Step 5 — Item-15 lock contract and bypass staging

The caller-held unique_lock capability, the immutable authored bypass plan captured before the controller lock, the single restart lock interval, hook-entry readiness false with exact per-slot staging witnesses, and failure withdrawing readiness plus disconnecting the possibly partial-frame stream under the same lock. Publishes readiness through one staged helper reached by all three old hostReady true paths.

Records closed (9): `D-DETERMINISTIC-SEAM`, `DEP-ITEM16`, `P-READINESS-PUBLISHERS`, `R-AUTHORED-PLAN`, `R-BYPASS-STAGED`, `R-CALLER-HELD`, `R-FAILURE-WITHDRAWS`, `R-RESTART-ORDER`, `R-SUPERSEDE-PASS3`

Tests landed (7): `T-EXACT-SLOTS`, `T-LOCK-CAPABILITY`, `T-OLD-GUARD-CONTROL`, `T-ORDER-CONTROL`, `T-RT-BEFORE-OFFLINE`, `T-SEND-FAILURE`, `T-STALE-PLAN`

### Step 6 — Dispatch protocol: ring batch atomicity, correlated replay, the mirror witness, the session block transaction, and both tickets

ringWriteBatch reserving a whole batch with one CAS and publishing the first reserved ready flag last; the ReplayCompletePayload gate and replayAckGate with the production no-JUCE receiver seam; the {dispatchTicket, mirrorEpoch, replayGate, causes, stage} witness with Unprimed/AwaitingAck/Complete/FailedPermanent; the {globalDeviceId, parameterUid16} mirror key; the two-phase SessionBlockPlan over every host event writer; and both tickets in one tuple. ONE STEP BECAUSE THE CONTRACT MAKES IT ONE: R-DISPATCH-TICKET's tuple names the sessionBlockTicket and mirror stage that R-TRANSACTIONAL-EVENT-BATCH and R-MIRROR-EPOCH introduce, while both of those records declare R-DISPATCH-TICKET as their dependency. No split of this cluster leaves a step in which any of its records is intact. Installs the P-DISPATCH-PROTOCOL-SURFACES census.

Records closed (10): `D-PRODUCTION-RECEIVER`, `P-DISPATCH-PROTOCOL-SURFACES`, `R-ATOMIC-PRIMER-CAPACITY`, `R-CORRELATED-REPLAY-ACK`, `R-DISPATCH-TICKET`, `R-MIRROR-EPOCH`, `R-MIRROR-INSTANCE-IDENTITY`, `R-PROTOCOL-VERSION`, `R-R13-RECONCILIATION`, `R-TRANSACTIONAL-EVENT-BATCH`

Tests landed (12): `T-ACK-BOUNDARY`, `T-ALL-EVENT-WRITERS`, `T-ALL-PUBLISHERS`, `T-BATCH-VISIBILITY`, `T-BYPASS-FAILURE`, `T-COLD-REFUSES`, `T-DUPLICATE-PLUGIN-MIRRORS`, `T-ORDINARY-BATCH-ATOMIC`, `T-PRIMER-CAPACITY`, `T-PROTOCOL-VERSIONS`, `T-RECEIVER-BOUND`, `T-SESSION-BLOCK-ATOMIC`

### Step 7 — Master correlation and the G4 witness

Master FX as a downstream participant of the same session block on the same dispatch helper and witness; the six-field handoff identity carried by TrackInfo, aux-child copies and master handoffs; and the producer transport and backpressure gates comparing all six. Retires masterFxActive as an execution authority, which is what lets the stale-authority scanner assert its complete forbidden set.

Records closed (2): `R-G4-WITNESS`, `R-MASTER-CORRELATION`

Tests landed (4): `T-G4-WITNESS`, `T-MASTER-CORRELATION`, `T-PRODUCER-GATES`, `T-STALE-SNAPSHOT-AUTHORITY`

### Step 8 — Offline coordinator phases, the PASS-4 gate, and the production fixture

The explicit MappingReady / ControlPreroll / ResetAcknowledged / Playing / CountedOutput phases on the real runOfflinePump seam, stale-mapping rejection in both preflight readers, counted output accepted only on full identity match, and renderFailed with no partial output. Lands the PASS-4 replacement in the SAME step because its own statement ends 'makes offline rendering fail without output' while R-OFFLINE-PRIMER declares R-PASS4-REPLACEMENT as a dependency — the two are mutually referential. Lands the clock-free production fixture, the routing fixture over acyclic/cyclic/diamond graphs, the artifact fixture, and the separate coordinator fixture.

Records closed (4): `D-PRODUCTION-FIXTURE`, `P-OFFLINE-OUTPUT-SURFACES`, `R-OFFLINE-PRIMER`, `R-PASS4-REPLACEMENT`

Tests landed (10): `T-CAPACITY-PERMANENT`, `T-LOWER-PRIMER`, `T-MAPPING-PREFLIGHT`, `T-NO-DRAIN-LEAK`, `T-OFFLINE-NO-LEAK`, `T-OFFLINE-PHASES`, `T-ORDINARY-CAPACITY-PERMANENT`, `T-PRIMED-NOT-COMPLETE`, `T-REARM-REGRESSES`, `T-TRACK-LOCAL`

## Records this plan does not land, and why

- `CTRL-MUTATIONS` — the packet's mutation self-test; re-run by the completion gate
- `CTRL-PACKET` — the packet checker; re-run by the completion gate, not implemented in the product
- `DEP-FROZEN-BASE` — pins the frozen product commit this branch starts from
- `DEP-ITEM15` — pins item-15's dual-PASS rulings as inputs; the rulings themselves are landed individually at step 5
- `DEP-ITEM18` — item-15's record that item 18 blocked implementation; discharged by item 18's dual PASS
- `DEP-PREDECESSOR` — pins an immutable input packet
- `E-PROBE-CONFOUNDER` — an evidence classification of an existing probe, not a behaviour to implement
- `G-ITEM15` — packet gate — a statement ABOUT the packet's own review state, not a product behaviour
- `G-ITEM18` — packet gate — same
- `P-GOVERNED-FILES` — the item-15 packet's own governed bytes and their digests; nothing in the product implements it
- `R-NO-AUTHORIZATION` — pure authorization; discharged by item 18's dual PASS before this branch existed
- `R-REVIEW-GATED-AUTH` — pure authorization; discharged by the same dual PASS

## Mutual references that force records to share a step

Each row is a place where a record's own frozen STATEMENT names a construct introduced by a record that declares it as a dependency. The declared graph is a DAG; these are the edges it does not carry, and they are what collapse the dispatch and offline clusters.

- `R-DISPATCH-TICKET` (step 6) needs `R-TRANSACTIONAL-EVENT-BATCH` (step 6): "Each ordinary SessionBlockPlan also receives one nonzero monotonic sessionBlockTicket shared by all of its participants." — the reverse of the declared edge; both records are therefore in the same step (6)
- `R-DISPATCH-TICKET` (step 6) needs `R-MIRROR-EPOCH` (step 6): "compare the same mapping/generation/host-ticket/session-ticket/revision plus mirror stage before using a host"
- `R-PASS4-REPLACEMENT` (step 8) needs `R-OFFLINE-PRIMER` (step 8): "makes offline rendering fail without output" — R-OFFLINE-PRIMER declares R-PASS4-REPLACEMENT as a dependency while this statement asserts an offline outcome, so the two are mutually referential and share step 8
- `R-PASS4-REPLACEMENT` (step 8) needs `R-MASTER-CORRELATION` (step 7): "and master processed-output publication for that host until the exact witness is Complete"
- `R-ATOMIC-PRIMER-CAPACITY` (step 6) needs `R-TRANSACTIONAL-EVENT-BATCH` (step 6): "Lesser N with transient occupancy follows R-TRANSACTIONAL-EVENT-BATCH's exact in-flight wait/retry rule and sends no drain." — same direction as the declared edge; both in step 6
- `R-CORRELATED-REPLAY-ACK` (step 6) needs `R-MIRROR-EPOCH` (step 6): "binds it to the current ticket/epoch" — declared edge too; both in step 6
- `R-ROUTING-AUTHORITY` (step 4) needs `R-HOST-PLAN-AUTHORITY` (step 4): "A routing command mutates only a candidate document and atomically publishes the compiled graph" — the atomicity half is inseparable from the ruling, so the whole record lands at step 4 and step 3 is preparatory code inside the same branch rather than a record boundary

## Test bindings

| test | step | records it needs | why |
|---|---|---|---|
| `T-ACK-BOUNDARY` | 6 | `R-CORRELATED-REPLAY-ACK`, `R-MIRROR-EPOCH` | the exact gate transitions the same ticket/epoch to Complete, so the epoch must exist |
| `T-ALL-EVENT-WRITERS` | 6 | `R-TRANSACTIONAL-EVENT-BATCH`, `P-DISPATCH-PROTOCOL-SURFACES` | all eight frozen ringWrite call sites classified, six migrated to staging |
| `T-ALL-PUBLISHERS` | 6 | `P-READINESS-PUBLISHERS`, `R-DISPATCH-TICKET` | each of the three hostReady paths reaches the one staged publication helper that publishes the ticket |
| `T-ARTIFACT-PRESENCE-MATRIX` | 4 | `R-DEVICE-ID-LIFETIME` | executes the four artifact_presence_matrix rows the id rule owns |
| `T-ARTIFACT-PROVENANCE` | 4 | `R-DEVICE-ID-LIFETIME` | the stale-canonical-file negative path of the same rule |
| `T-BATCH-VISIBILITY` | 6 | `R-ATOMIC-PRIMER-CAPACITY` | a consumer observes no batch entry until the first reserved ready flag publishes last |
| `T-BYPASS-FAILURE` | 6 | `R-FAILURE-WITHDRAWS`, `R-DISPATCH-TICKET` | asserts the ticket is left ZERO, which is the ticket's own state |
| `T-CAPACITY-PERMANENT` | 8 | `R-ATOMIC-PRIMER-CAPACITY`, `R-PASS4-REPLACEMENT`, `R-OFFLINE-PRIMER` | asserts offline render terminates with renderFailed and no output, so the offline coordinator's terminal outcome must exist |
| `T-COLD-REFUSES` | 6 | `R-DISPATCH-TICKET` | a zero dispatch ticket refuses every ProcessBlock and audio mapping read |
| `T-DEVICE-ID-LIFETIME` | 4 | `R-DEVICE-ID-LIFETIME`, `R-HOST-PLAN-AUTHORITY` | asserts candidates 'fail before publication without changing the prior snapshot', so it needs the snapshot publication transaction, not only the id rule |
| `T-DUPLICATE-PLUGIN-MIRRORS` | 6 | `R-MIRROR-INSTANCE-IDENTITY`, `R-MIRROR-EPOCH`, `R-ATOMIC-PRIMER-CAPACITY` | each batch entry resolves to the correct track plan and distinct compact index before atomic reservation |
| `T-EXACT-SLOTS` | 5 | `R-BYPASS-STAGED`, `R-AUTHORED-PLAN`, `R-HOST-PLAN-AUTHORITY` | BOTH manifests declare this id and their statements differ in population; the implementation lands both — item-15's 'per hosted VST slot and no attempt for non-host devices' AND item-18's 'per resolvable VST slot before the ticket publishes, non-VST topology produces no SetBypass, hostless aux produces neither'. Neither is assumed to contain the other |
| `T-G4-WITNESS` | 7 | `R-G4-WITNESS`, `R-MASTER-CORRELATION` | TrackInfo, aux-child copies and master handoffs compare all six fields |
| `T-GLOBAL-DEVICE-IDENTITY` | 4 | `R-DEVICE-ID-LIFETIME`, `R-STABLE-DEVICE-TARGETS`, `R-PROJECT-TARGET-MIGRATION` | names 'plugin-state blob, and parameter-manifest identity' alongside automation and mirror identity, so it spans the id rule, the durable-target rule and the schema-6 target tag |
| `T-LEGACY-DISABLED-ROUNDTRIP` | 4 | `R-PROJECT-TARGET-MIGRATION` | old-load to schema-6-save to schema-6-load of a DisabledLegacyCompact tag |
| `T-LOCK-CAPABILITY` | 5 | `R-CALLER-HELD`, `D-DETERMINISTIC-SEAM` | the staging seam receives an owning unique_lock for the exact runtime mutex |
| `T-LOWER-PRIMER` | 8 | `R-PASS4-REPLACEMENT`, `R-MIRROR-EPOCH`, `R-ATOMIC-PRIMER-CAPACITY` | ordinary work, active, track output and master processed output must all be absent, which is the PASS-4 gate |
| `T-MAPPING-PREFLIGHT` | 8 | `R-OFFLINE-PRIMER` | awaitAnyReadyTrack and awaitAllReadyTracks reject a stale mapping exactly as process and awaitNextBlock do |
| `T-MASTER-CORRELATION` | 7 | `R-MASTER-CORRELATION`, `R-G4-WITNESS` | restart or re-arm between processing and publication rejects the old handoff on the six-field identity |
| `T-NO-DRAIN-LEAK` | 8 | `R-TRANSACTIONAL-EVENT-BATCH`, `R-PASS4-REPLACEMENT` | transient occupancy never emits a drain ProcessBlock |
| `T-OFFLINE-NO-LEAK` | 8 | `R-OFFLINE-PRIMER`, `P-OFFLINE-OUTPUT-SURFACES` | the real coordinator completes pre-roll while stopped and produces tick-zero as counted block one |
| `T-OFFLINE-PHASES` | 8 | `R-OFFLINE-PRIMER`, `R-MASTER-CORRELATION`, `D-PRODUCTION-FIXTURE` | refuses Playing before all track and master witnesses are Complete; driven by the separate coordinator fixture |
| `T-OLD-GUARD-CONTROL` | 5 | `R-SUPERSEDE-PASS3`, `DEP-ITEM16` | restoring the old if-not-hostReady early return must delete every staging witness |
| `T-ORDER-CONTROL` | 5 | `R-SUPERSEDE-PASS3`, `R-RESTART-ORDER`, `P-READINESS-PUBLISHERS` | hook-entry readiness false, exact staging witnesses, readiness published only after them |
| `T-ORDINARY-BATCH-ATOMIC` | 6 | `R-TRANSACTIONAL-EVENT-BATCH`, `R-ATOMIC-PRIMER-CAPACITY` | every participant batch commits only inside the session two-phase protocol |
| `T-ORDINARY-CAPACITY-PERMANENT` | 8 | `R-TRANSACTIONAL-EVENT-BATCH`, `R-OFFLINE-PRIMER` | same: 'makes offline rendering set renderFailed with zero partial output' |
| `T-PLAN-RACE` | 4 | `R-HOST-PLAN-AUTHORITY`, `R-DEVICE-ID-LIFETIME`, `R-ROUTING-AUTHORITY` | one publication switches the device map/watermark, routing, patcher ownership, automation tags and master together |
| `T-PRIMED-NOT-COMPLETE` | 8 | `R-PASS4-REPLACEMENT`, `R-MIRROR-EPOCH` | a second producer step emits neither a second primer nor an ordinary sentinel |
| `T-PRIMER-CAPACITY` | 6 | `R-ATOMIC-PRIMER-CAPACITY`, `R-TRANSACTIONAL-EVENT-BATCH` | the transient-occupancy half follows R-TRANSACTIONAL-EVENT-BATCH's exact in-flight wait/retry rule by that record's own words, so it cannot close before the session transaction exists |
| `T-PRODUCER-GATES` | 7 | `R-G4-WITNESS`, `R-DISPATCH-TICKET`, `R-MIRROR-EPOCH` | both producer gates compare the full tuple |
| `T-PROJECT-TARGET-MIGRATION` | 4 | `R-PROJECT-TARGET-MIGRATION`, `R-DEVICE-ID-LIFETIME` | round-trips next_device_id, artifact_generation and artifact_entries together with the tagged targets |
| `T-PROTOCOL-VERSIONS` | 6 | `R-PROTOCOL-VERSION`, `R-CORRELATED-REPLAY-ACK`, `R-DEVICE-ID-LIFETIME` | asserts kControlVersion 15, kShmVersion 42, schema 6 fields, the payload/mailbox sizes AND that global id 0x7FFF is lossless |
| `T-REARM-REGRESSES` | 8 | `R-MIRROR-EPOCH`, `R-PASS4-REPLACEMENT` | overflow re-arm returns readiness to Unprimed and refuses the sentinel again |
| `T-RECEIVER-BOUND` | 6 | `D-PRODUCTION-RECEIVER`, `R-ATOMIC-PRIMER-CAPACITY`, `R-CORRELATED-REPLAY-ACK` | the real ringWriteBatch sender feeds the no-JUCE helper handleProcessBlock calls |
| `T-ROUTING-ATOMICITY` | 4 | `R-ROUTING-AUTHORITY`, `R-HOST-PLAN-AUTHORITY` | a mutation parked between candidate compilation and publication |
| `T-ROUTING-BLOCK-DETERMINISM` | 4 | `R-ROUTING-AUTHORITY`, `R-HOST-PLAN-AUTHORITY` | asserts that reordering runtime storage or worker completion cannot change delivery, which is runtime behaviour reading the published graph rather than the normalizer alone |
| `T-ROUTING-MATRIX` | 4 | `R-ROUTING-AUTHORITY`, `R-HOST-PLAN-AUTHORITY` | iterates the exact 5x4 matrix; validity is decided during candidate compilation, which is the snapshot transaction |
| `T-RT-BEFORE-OFFLINE` | 5 | `R-CALLER-HELD`, `D-DETERMINISTIC-SEAM` | realtime try_lock reports failure before an offline blocking waiter acquires |
| `T-SEND-FAILURE` | 5 | `R-FAILURE-WITHDRAWS`, `R-BYPASS-STAGED` | partial-frame failure withdraws readiness and disconnects under the same lock |
| `T-SESSION-BLOCK-ATOMIC` | 6 | `R-TRANSACTIONAL-EVENT-BATCH`, `R-DISPATCH-TICKET` | the two-phase protocol over two routed hosts |
| `T-STABLE-DEVICE-TARGETS` | 4 | `R-STABLE-DEVICE-TARGETS`, `R-HOST-PLAN-AUTHORITY` | plan-local compact indexes change while stable targets do not, which needs the plan to exist |
| `T-STALE-PLAN` | 5 | `R-AUTHORED-PLAN`, `R-HOST-PLAN-AUTHORITY` | a changed plan identity prevents readiness publication for the stale plan |
| `T-STALE-SNAPSHOT-AUTHORITY` | 7 | `R-HOST-PLAN-AUTHORITY`, `R-MASTER-CORRELATION`, `P-EXECUTION-AUTHORITY-CONSUMERS` | its forbidden set includes masterFxActive, which stays an execution authority until R-MASTER-CORRELATION retires it; asserting the set earlier would assert an incomplete set |
| `T-STATE-ARTIFACT-MIGRATION` | 4 | `R-DEVICE-ID-LIFETIME` | the artifact half of the id rule: legacy key read, generation commit, byte-identical restore |
| `T-TRACK-LOCAL` | 8 | `R-PASS4-REPLACEMENT`, `R-TRANSACTIONAL-EVENT-BATCH` | a parked primer never joins or blocks the ordinary SessionBlockPlan |

## Gate at every step

- cmake --build build is clean, checked as grep -c 'error:' == 0 on the build log, read before anything is committed.
- The full ctest population PASSES with no new failure relative to the frozen baseline, and any pre-existing failure is named and attributed to the baseline by reproducing it in the frozen product worktree.
- Every test the step map binds to THIS step passes. A test bound to a later step is not expected to exist yet and is not claimed to.
- An independent sub-agent has reviewed the step's diff against the frozen records it touches.

## Completion gate for the branch

- Every record in record_steps is landed intact; every record in records_not_landed is accounted for by its stated reason.
- All 45 distinct test ids (39 item-18 + 7 item-15, T-EXACT-SLOTS shared and landed as BOTH statements) exist as executable tests and PASS.
- cmake --build build is clean: grep -c 'error:' over the build log is 0.
- The full ctest population PASSES unfiltered, with no exclusion filter, and the Rust suites pass.
- python3 tools/architecture/ae_p1_2_g2b_item15_check.py still PASSES in the item-15 packet worktree, and its 16/16 mutations are still refused.
- python3 tools/architecture/ae_p1_2_g2b_item18_check.py still PASSES in the item-18 packet worktree, with 33 records, 39 tests, 20 routing rows, 4 artifact presence rows, 89 governed files, 17 state-artifact sites, 100 gates and 118/118 mutation controls refused.
- kControlVersion is 15 and kShmVersion is 42 in C++, Rust and SHM_LAYOUT.md together, and the existing size/offset assertions still hold.
- The project schema is 6 and carries next_device_id, tagged automation targets, artifact_generation and artifact_entries.
