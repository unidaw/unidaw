# AE-P1.2 G2-B — item 15 lock-contract successor

> Generated from `AE-P1.2-g2b-item15-manifest.json`; do not edit by hand.

Status: `REVIEW_CANDIDATE`. Owner: `backend`.
Frozen product: `92dfdfe23cc7ff93f2ce14894a35d089e3d9e2b8` (tree `238ac970b5d61fe16055ede4c43a2978ddb11da7`).
Program source: `02e984f578d1e08ff0773c354ce87aa7826f7f06` (tree `b7965c847d40ec8e4ef5b19359782ac28e49e4c7`).
Successor to packet `2b5f0747f1b7dde79ae788af3826c49c78df5d2a` / manifest `c321130b860fda73991f04d1035bea7af03faf6e030fce5565664c1657ce093e`.
Reopening reason: The settled predecessor identifies the self-deadlock but leaves G2-B planning unknown because it does not choose a lock contract. The later bounded-send probe is non-discriminating: ProcessBlock can fail first and withdraw hostReady before the bypass command reaches its send.

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
- `DEP-ITEM18` [dependency / BLOCKING_IMPLEMENTATION]: Item 18 still lacks an executable PASS replacement for the ruled readiness model; item 15 can be decided without pretending that implementation is authorized.
- `E-PROBE-CAUSALITY` [evidence / DERIVED]: The probe deliberately runs ProcessBlock traffic for five seconds against a stopped host. A failed ProcessBlock clears hostReady, so the later bypass handler can return at its guard without calling sendSetBypass. The blocking and bounded variants therefore measure the same non-send path.
- `R-CALLER-HELD` [ruling / DECIDED]: The later staging helper takes an owning std::unique_lock<std::mutex>& capability for runtime.controllerMutex, verifies owns_lock() and mutex() identity, and never locks controllerMutex internally. A bool saying the lock is held is forbidden.
- `R-AUTHORED-PLAN` [ruling / DECIDED]: Bypass values come from one immutable authored plan with an identity or monotonic revision. The plan is captured before controllerMutex is acquired; staging never takes trackMutex under controllerMutex. A changed plan cannot publish readiness for the stale one. Item 18 must choose the publication representation and chain-edit linearization.
- `R-RESTART-ORDER` [ruling / DECIDED]: On restart, one controller lock ownership interval covers launch, host-generation publication, watchdog installation, bypass staging, and the readiness publication authorized by item 18. The helper is called inside that interval, so reacquiring the mutex would self-deadlock.
- `R-FAILURE-WITHDRAWS` [ruling / DECIDED]: Every sendSetBypass result is consumed. False leaves readiness withdrawn and requests recovery. Because sendMessageNonBlocking can return false after a partial SOCK_STREAM frame, that controller connection is transport-poisoned and must not carry a later frame before disconnect/relaunch.
- `R-NO-AUTHORIZATION` [ruling / DECIDED]: This successor closes only item 15's planning choice. It cannot authorize product work until item 18 supplies an executable acceptance oracle and the successor inventories every readiness-true and chain-edit publication site.
- `D-DETERMINISTIC-SEAM` [test_decision / PLANNED]: The implementation test uses a fake staging sender and barriers, not socket-buffer timing: it proves lock ownership and ordering, RT try-lock behavior before an offline waiter, exact per-slot staging, stale-plan refusal, and failure withdrawal/transport poison.
- `CTRL-PACKET` [control / EXECUTABLE]: python3 tools/architecture/ae_p1_2_g2b_item15_check.py verifies identities, governed bytes, source locators, dependency closure, causal anchors, and byte-identical generated prose.
- `CTRL-MUTATIONS` [control / EXECUTABLE]: The same checker self-tests removal of the caller-held ruling, false implementation authorization, omission of the partial-frame case, and a broken dependency. Each mutation must be refused.

## Required future test cases

- `T-LOCK-CAPABILITY`: The staging seam receives an owning unique_lock for the exact runtime mutex and reaches its barrier while that lock remains owned; adding an internal lock prevents the barrier and fails.
- `T-RT-BEFORE-OFFLINE`: While staging is parked, a realtime try_lock probe reports failure promptly before an offline blocking waiter is allowed to acquire after release.
- `T-EXACT-SLOTS`: A multi-slot authored plan produces exactly one ordered SetBypass attempt per hosted VST slot and no attempt for non-host devices.
- `T-STALE-PLAN`: Changing the authored plan identity while staging is parked prevents readiness publication for the stale plan.
- `T-SEND-FAILURE`: A forced SetBypass false result leaves readiness withdrawn, marks recovery needed, and marks the stream unusable until disconnect/relaunch.
- `T-ORDER-CONTROL`: Moving readiness publication before staging makes the deterministic ordering assertion fail.

## Non-goals

- No product source edit, build, or runtime test is authorized by this packet.
- No claim that tools/bypass_send_probe.sh demonstrates the blocking-send defect.
- No decision that mirror replay is part of, or orthogonal to, the eventual readiness representation; item 18 owns that acceptance question.
- No conversion of other blocking HostController messages to the bounded sender.

## Review requirement

This packet may close item 15's planning choice only after independent semantic and evidence reviewers both return PASS for the same immutable packet SHA and frozen product base. Any product implementation requires a separate reviewed successor and implementation ticket after the two blockers above are resolved.
