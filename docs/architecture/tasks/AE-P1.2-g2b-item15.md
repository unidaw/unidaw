# AE-P1.2 G2-B — item 15 lock-contract successor

> Generated from `AE-P1.2-g2b-item15-manifest.json`; do not edit by hand.

Status: `REVIEW_CANDIDATE`. Owner: `backend`.
Frozen product: `92dfdfe23cc7ff93f2ce14894a35d089e3d9e2b8` (tree `238ac970b5d61fe16055ede4c43a2978ddb11da7`).
Program source: `02e984f578d1e08ff0773c354ce87aa7826f7f06` (tree `b7965c847d40ec8e4ef5b19359782ac28e49e4c7`).
Successor to packet `2b5f0747f1b7dde79ae788af3826c49c78df5d2a` / manifest `c321130b860fda73991f04d1035bea7af03faf6e030fce5565664c1657ce093e`.
Revision successor to `4a70972ac468d7c1320e95e940b3d4fbcbdd829c` / manifest `2c31f168c841f6a81c8f091283092108f9bec7fac1e80b05a92a7de9db3842cb`.
Reopening reason: The first focused successor at 4a70972a was blocked by both independent reviews: it left manifest and packet locators unresolved, promoted a permitted probe confounder into observed history, contradicted predecessor PASS 3 without superseding it or item 16, and did not prove a poisoned partial stream is disconnected before an already-waiting offline dispatcher acquires the controller lock.

## Scope

Resolve AE-P1.2 open item 15 by choosing the lock ownership, authored-state source, ordering, and failure contract that a later G2-B bypass-readiness implementation must obey.

Implementation authorized: `false`.

## Blocked by

- AE-P1.2 item 18: executable acceptance for the ruled two-level readiness model
- A complete chain-edit linearization and hostReady publication-site census

## Records

- `G-ITEM15` [gate / READY_FOR_REVIEW]: Item 15 is decided for planning: bypass staging never reacquires controllerMutex; the caller supplies and retains the controller lifetime lock through staging. This packet does not authorize the product change.
- `DEP-PREDECESSOR` [dependency / PINNED]: The predecessor packet commit, tree, and manifest bytes are immutable inputs.
- `DEP-FROZEN-BASE` [dependency / PINNED]: The packet describes exactly the frozen product commit, tree, and governed file bytes.
- `DEP-ITEM16` [dependency / SUPERSEDED_HERE]: Item 16 owns the old guard/swap trap. This successor necessarily resolves it with item 15: the hook enters while readiness is withdrawn, contains no hostReady early return, and is witnessed by exact staging attempts.
- `DEP-ITEM18` [dependency / BLOCKING_IMPLEMENTATION]: Item 18 still lacks an executable PASS replacement for the ruled readiness model; item 15 can be decided without pretending that implementation is authorized.
- `E-PROBE-CONFOUNDER` [evidence / DERIVED_CONFOUNDER]: The probe runs ProcessBlock traffic for five seconds against a stopped host before issuing bypass. Product code permits a failed ProcessBlock to clear hostReady, after which the later bypass handler can return at its guard without calling sendSetBypass; the restart worker can also republish readiness during that delay. These are permitted false-green paths, so the historical timings do not discriminate the sender. This identifies confounders; it does not establish which path either historical run executed.
- `R-CALLER-HELD` [ruling / DECIDED]: The later staging helper takes an owning std::unique_lock<std::mutex>& capability for runtime.controllerMutex, verifies owns_lock() and mutex() identity, and never locks controllerMutex internally. A bool saying the lock is held is forbidden.
- `R-AUTHORED-PLAN` [ruling / DECIDED]: Bypass values come from one immutable authored plan with an identity or monotonic revision. The plan is captured before controllerMutex is acquired; staging never takes trackMutex under controllerMutex. A changed plan cannot publish readiness for the stale one. Item 18 must choose the publication representation and chain-edit linearization.
- `R-SUPERSEDE-PASS3` [ruling / DECIDED]: Predecessor PASS 3 is superseded. hookEntryHostReady must be FALSE: readiness stays withdrawn until bypass staging succeeds. Recovery is proved to execute by exact per-slot staging witnesses, and restoring the old if-not-hostReady early return makes those witnesses absent and fails the control.
- `R-RESTART-ORDER` [ruling / DECIDED]: On restart, one controller lock ownership interval covers launch, host-generation publication, watchdog installation, bypass staging, and the readiness publication authorized by item 18. Readiness remains withdrawn at hook entry and until staging succeeds. The helper is called inside that interval, so reacquiring the mutex would self-deadlock.
- `R-FAILURE-WITHDRAWS` [ruling / DECIDED]: Every sendSetBypass result is consumed. False leaves readiness withdrawn, requests recovery, and disconnects the transport-poisoned controller under the same controller lock before any waiter can acquire it. Because sendMessageNonBlocking can return false after a partial SOCK_STREAM frame, no later frame may use that connection before relaunch.
- `R-NO-AUTHORIZATION` [ruling / DECIDED]: This successor closes only item 15's planning choice. It cannot authorize product work until item 18 supplies an executable acceptance oracle and the successor inventories every readiness-true and chain-edit publication site.
- `D-DETERMINISTIC-SEAM` [test_decision / PLANNED]: The implementation test uses a fake staging sender and barriers, not socket-buffer timing: it proves lock ownership and ordering, the superseded guard control, RT try-lock behavior before an offline waiter, exact per-slot staging, stale-plan refusal, and failure withdrawal plus disconnect before a stale offline waiter can dispatch.
- `CTRL-PACKET` [control / EXECUTABLE]: python3 tools/architecture/ae_p1_2_g2b_item15_check.py verifies identities, governed bytes, resolves frozen/predecessor ranges and manifest/packet locators, checks dependency closure and bounded confounder anchors, and enforces byte-identical generated prose.
- `CTRL-MUTATIONS` [control / EXECUTABLE]: The same checker self-tests removal of the caller-held ruling, false implementation authorization, omission of the partial-frame case, a broken dependency, a missing manifest pointer, an absent packet path, probe overclaim, and loss of the PASS 3 supersession. Each mutation must be refused.

## Required future test cases

- `T-LOCK-CAPABILITY`: The staging seam receives an owning unique_lock for the exact runtime mutex and reaches its barrier while that lock remains owned; adding an internal lock prevents the barrier and fails.
- `T-RT-BEFORE-OFFLINE`: While staging is parked, a realtime try_lock probe reports failure promptly before an offline blocking waiter is allowed to acquire after release.
- `T-EXACT-SLOTS`: A multi-slot authored plan produces exactly one ordered SetBypass attempt per hosted VST slot and no attempt for non-host devices.
- `T-STALE-PLAN`: Changing the authored plan identity while staging is parked prevents readiness publication for the stale plan.
- `T-SEND-FAILURE`: An offline dispatcher reads ready and parks on controllerMutex; staging then forces a partial-frame SetBypass failure, withdraws readiness, requests recovery, and disconnects under that same lock. After release the waiter acquires but sends no ProcessBlock before relaunch.
- `T-ORDER-CONTROL`: hookEntryHostReady is false, exact staging witnesses occur, and readiness publishes only after them; moving publication before staging fails the ordering assertion.
- `T-OLD-GUARD-CONTROL`: Restoring the old if-not-hostReady early return removes every exact staging witness at hook entry and fails, proving the superseded swap trap cannot silently delete recovery.

## Non-goals

- No product source edit, build, or runtime test is authorized by this packet.
- No claim that tools/bypass_send_probe.sh demonstrates the blocking-send defect.
- No decision that mirror replay is part of, or orthogonal to, the eventual readiness representation; item 18 owns that acceptance question.
- No conversion of other blocking HostController messages to the bounded sender.

## Review requirement

This packet may close item 15's planning choice only after independent semantic and evidence reviewers both return PASS for the same immutable packet SHA and frozen product base. Any product implementation requires a separate reviewed successor and implementation ticket after the two blockers above are resolved.
