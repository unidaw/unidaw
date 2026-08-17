# AE-P1.2 G2-B — item 18 readiness successor

> Generated from `AE-P1.2-g2b-item18-manifest.json`; do not edit by hand.

Status: `REVIEW_CANDIDATE`. Owner: `backend`.
Frozen product: `92dfdfe23cc7ff93f2ce14894a35d089e3d9e2b8` (tree `238ac970b5d61fe16055ede4c43a2978ddb11da7`).
Item-15 input: `8ee5b3cdd34ef6c5538fac19074b4f442c0a8514` / manifest `a9583a4cb8a8fd45d1dc2ccfb683cf06f8758c55407bfdf6f3d5a424eea4465f`.
Revision successor to: `886544f1b933007021c1cb7a7bee65ea982fcf7f` / manifest `706353c3e8421c280e681b49ebee0884bf9343ce0c9ec87f965c930f027d03f8`.

## Scope

Close AE-P1.2 G2-B item 18 by replacing withdrawn PASS 4 with an executable production-bound oracle, defining a re-entrant mirror-complete observation, and binding every hosted execution-plan mutation and readiness publication to the item-15 lock and failure contract.

Implementation authorized before dual PASS: `false`. The same-SHA dual PASS authorizes the declared scope: `true`.

## Frozen populations

- Readiness-true publishers: `3`.
- TrackStateSnapshot publications: `24`.
- Hosted-plan mutation roots: `16`.
- Explicit non-host mutation families: `4`.
- Mechanically reproduced lexical mutation candidates: `44`.

## Review history

- `886544f1b933007021c1cb7a7bee65ea982fcf7f`: semantic `BLOCKED`, evidence `BLOCKED`. Schema v2 adds the omitted setDevicePatcherNodeId execution-plan mutation and Euclidean internal family, then replaces self-declared completeness with a 44-line frozen-source lexical scan and deletion/substitution controls. It also makes mirror transitions one controller-locked ticket/epoch transaction and adds fail-closed exhaustion and primer-capacity requirements.

## Records

- `G-ITEM18` [gate / READY_FOR_REVIEW]: Item 18 is acceptance-decidable: PASS 4 is replaced by a clock-free production-bound primer/ack/re-arm oracle, and every readiness publisher plus hosted execution-plan mutation is bound to the item-15 staging contract.
- `DEP-PREDECESSOR` [dependency / PINNED]: The original AE-P1.2 packet and its withdrawn PASS 4 are immutable inputs.
- `DEP-ITEM15` [dependency / DUAL_PASS_PINNED]: The dual-PASS item-15 lock, plan-capture, bypass-failure, poisoned-transport, and stale-waiter rulings are mandatory implementation inputs.
- `DEP-FROZEN-BASE` [dependency / PINNED]: The census and acceptance design describe exactly frozen product 92dfdfe2 and its governed blobs.
- `P-READINESS-PUBLISHERS` [population / EXACT_3]: Exactly three production hostReady true stores exist; none proves mapping plus successful bypass staging for a published runtime.
- `P-SNAPSHOT-PUBLISHERS` [population / EXACT_24]: Exactly twenty-four production TrackStateSnapshot publications exist: three prepublication assignments and twenty-one atomic stores; this packet does not silently treat them as a coherent host-plan authority.
- `P-HOST-PLAN-MUTATIONS` [population / EXACT_CLASSIFIED_ROOTS]: The hosted execution plan is the full ordered device topology needed to derive VST segments, including resolvable VST identity/slot/bypass and PatcherAudio bypass/node identity. Sixteen semantic roots and four classified internal-state families are cross-checked against a mechanically reproduced 44-line frozen-source lexical scan; hostless aux, document copies, and internal sampler/patcher/Euclidean mutations remain explicit.
- `R-BYPASS-STAGED` [ruling / DECIDED]: The lower stage is MappedAndBypassStaged, not a claim that the plugin acknowledged application. It is reached only after every ordered SetBypass frame was transmitted completely; stream order places those frames before ProcessBlock. Any false result withdraws readiness and disconnects the possibly partial-frame-poisoned stream under controllerMutex.
- `R-HOST-PLAN-AUTHORITY` [ruling / DECIDED]: A dedicated immutable AuthoredHostExecutionPlan is the sole hosted-segmentation authority. It carries a monotonic nonzero uint64 revision and the exact full ordered topology, resolved VST slots/bypass, and PatcherAudio node identities. Every included mutation publishes a new plan before reconciliation. The producer loads it, acquires controllerMutex, reloads the current plan, and refuses unless both identities and the staged revision still match; TrackStateSnapshot.chainDevices is no longer an authority for hosted segmentation. Revisions never wrap: exhaustion refuses the edit and leaves the prior plan authoritative.
- `R-DISPATCH-TICKET` [ruling / DECIDED]: One nonzero uint64 dispatch ticket binds the current mapping, exact staged host-plan revision, and successful bypass staging. Zero is withdrawn. Publication occurs under controllerMutex after the mirror decision; producer dispatch rechecks it after acquiring that mutex. TrackInfo snapshots the ticket with the mapping and compares it to the live ticket, so a stale offline waiter or audio mapping cannot retain authorization across withdrawal or relaunch. Tickets never wrap: exhaustion stays withdrawn and reports a terminal refusal.
- `R-MIRROR-EPOCH` [ruling / DECIDED]: Mirror readiness is a re-entrant generation, not a monotonic startup enum. One controllerMutex-guarded witness contains the exact dispatch ticket, a nonzero uint64 mirror epoch, and Pending or Complete stage; no loose atomic pair is treated as a transaction. Every relaunch decision or overflow re-arm advances the epoch to Pending under the same owning lock; priming records that exact ticket, epoch, cause set, and nonzero gate; only ack >= that gate while the same ticket/epoch is still current may transition to Complete under the lock. A later re-arm therefore demotes readiness and a stale ack cannot promote it. Epochs never wrap: exhaustion withdraws the ticket and reports terminal refusal.
- `R-R13-RECONCILIATION` [ruling / DECIDED]: R13 remains valid: mirror parameters and ReplayComplete precede their primer ProcessBlock on the same ordered ring, and item 18 does not claim an intra-block parameter race. The new wait for the exact acknowledgement is the ruled observable MirrorComplete boundary and future G4 identity, not a repair for that withdrawn race theory. Gating is per host; the old global mirrorOnly scan is removed.
- `R-PASS4-REPLACEMENT` [ruling / DECIDED]: PASS 4 is replaced: a valid lower-stage ticket permits only a control-only primer ProcessBlock for the affected host while mirror Pending; ordinary MIDI, automation, render work, active publication, and output mixing for that host remain refused until the exact gate ack publishes Complete. Primer and drain outputs are discarded. The producer preflights capacity for every mirror entry plus ReplayComplete and publishes no partial primer; insufficient capacity sends only a drain block, leaves the epoch unprimed, and retries. Ack at gate-minus-one stays Pending, ack at gate completes, and Overflow re-arm returns to Pending. Unrelated tracks continue throughout.
- `R-OFFLINE-PRIMER` [ruling / DECIDED]: Offline rendering performs the same production primer exchange before timeline block zero. Primer blocks do not advance musical transport, consume a rendered block id, or enter output; the first counted/output block is the first ordinary block after Complete. Realtime primers are per-host and never stop unrelated tracks.
- `R-G4-WITNESS` [ruling / DECIDED_FOR_SUCCESSOR]: For later G4 work, readiness is a witness value, not a startup-only ordinal: {dispatchTicket, mirrorEpoch, mirrorStage}. The G4 dispatch identity must carry that witness, so Pending and Complete dispatches at the same block and segment cannot share an acknowledgement identity.
- `D-PRODUCTION-FIXTURE` [test_decision / PLANNED]: One clock-free fixture drives the production branch that calls sendProcessBlock with fake controller/mailbox barriers. It is parameterized for realtime and offline, covers all three old readiness publishers, every included live execution-plan class and the hostless negative classes, and emits exact stage, plan, slot, capacity, primer, ack, ordinary-event, active, dispatch, disconnect, and output witnesses. The checker, not this runtime fixture, classifies all 44 lexical candidates including document and internal-state paths.
- `R-REVIEW-GATED-AUTH` [authorization / CONDITIONAL]: Product implementation is unauthorized before independent semantic and evidence PASS results name this same immutable packet SHA and frozen product. Those two PASS results authorize only the declared item-15 plus item-18 scope.
- `CTRL-PACKET` [control / EXECUTABLE]: The checker binds external identities, all governed blobs, exact populations, source locators, dependency closure, authorization semantics, and generated prose.
- `CTRL-MUTATIONS` [control / EXECUTABLE]: The checker self-tests semantic and lexical population deletion/substitution, stale-plan omission, loose mirror publication, counter wrap, premature readiness, bypass overclaim, primed-is-complete, partial primer publication, stale ack, lost regression, global gating, offline leakage, authorization drift, locator substitution, and governed-byte drift.

## Required implementation tests

- `T-COLD-REFUSES`: A zero dispatch ticket refuses every ProcessBlock and every audio mapping read.
- `T-ALL-PUBLISHERS`: Each of the three old hostReady true paths reaches the one staged publication helper; deleting any path from the fixture population fails.
- `T-PLAN-RACE`: An offline dispatcher that loaded plan N and parked on controllerMutex refuses after plan N+1 is published or staged; only an exact post-lock plan identity and staged revision can dispatch.
- `T-EXACT-SLOTS`: Every included multi-slot execution plan produces exactly one ordered SetBypass attempt per resolvable VST slot before the ticket publishes; non-VST topology affects segmentation but produces no SetBypass, and hostless aux produces neither host dispatch nor bypass.
- `T-BYPASS-FAILURE`: A forced partial-frame SetBypass failure leaves the ticket zero, requests recovery, disconnects under controllerMutex, and a stale waiter sends no ProcessBlock after acquiring.
- `T-LOWER-PRIMER`: With a non-empty mirror and ack parked below gate, the actual production sender emits a control-only ProcessBlock carrying all mirror params plus ReplayComplete while ordinary work is absent, active stays false, and primer output is discarded for that host.
- `T-PRIMED-NOT-COMPLETE`: A second producer step with mirror primed and ack still below gate remains Pending and emits no ordinary sentinel.
- `T-PRIMER-CAPACITY`: With capacity below mirror-entry-count plus ReplayComplete, the producer publishes none of the primer, sends only a drain ProcessBlock, leaves the exact epoch unprimed, and retries successfully after capacity becomes sufficient.
- `T-ACK-BOUNDARY`: Ack equal to gate-minus-one remains Pending; ack equal to the exact nonzero gate transitions the same ticket and epoch to Complete under controllerMutex; only the next dispatch may carry the ordinary sentinel.
- `T-REARM-REGRESSES`: Overflow re-arm advances the epoch, returns readiness to Pending, refuses the sentinel again, and cannot be completed by the prior epoch's ack.
- `T-TRACK-LOCAL`: A parked Pending primer on track A never suppresses ordinary dispatch and output for ready track B.
- `T-OFFLINE-NO-LEAK`: Offline primers occur before timeline block zero and do not change transport, rendered-block count, first output block, or captured output.
- `T-G4-WITNESS`: Two otherwise equal dispatch identities with different mirror epoch or stage compare unequal, while identical readiness witnesses compare equal.

## Non-goals

- No product source is changed by this packet.
- No SetBypass host-applied acknowledgement is added; successful full-frame transmission on the ordered control stream is the defined staging boundary.
- No global mirrorOnly gate may silence unrelated tracks.
- Sampler document internals and patcher graph internals do not belong to the hosted execution plan, but their mutation families remain explicitly classified; chain topology and patcherNodeId do belong because they determine VST segment boundaries and post-segment audio nodes.
- The general lost-update problem among non-host TrackStateSnapshot publishers is recorded but is not solved by using that snapshot as a second host-plan authority.
- No SHM or wire-layout change is required for item 18; G4 may later carry the readiness witness in its dispatch identity.

## Review requirement

Independent semantic and evidence reviewers must both return PASS for the same immutable packet SHA and frozen product before the declared implementation authorization becomes effective.
