# AE-P1.2 G2-B — item 15 lock-contract successor

> Generated from `AE-P1.2-g2b-item15-manifest.json`; do not edit by hand.

Status: `REVIEW_CANDIDATE`. Owner: `backend`.
Frozen product: `92dfdfe23cc7ff93f2ce14894a35d089e3d9e2b8` (tree `238ac970b5d61fe16055ede4c43a2978ddb11da7`).
Program source: `02e984f578d1e08ff0773c354ce87aa7826f7f06` (tree `b7965c847d40ec8e4ef5b19359782ac28e49e4c7`).
Successor to packet `2b5f0747f1b7dde79ae788af3826c49c78df5d2a` / manifest `c321130b860fda73991f04d1035bea7af03faf6e030fce5565664c1657ce093e`.
Revision successor to `1f86e0015f83c666bb2f925eaeb6105a6011b622` / manifest `0c37a0f222d27292218c26e00cdaa0f34884d270105bed109b038073c13317c4`.
Reopening reason: The semantic review passed 1f86e001, but evidence review found that governed-file membership and per-record source-span membership could still shrink or substitute. The owner also found external commit fields were verified only against their self-declared trees, not the intended identities. This third consecutive locator/binding failure triggers the structural-fix rule: schema v3 hardcodes every external identity, the exact governed population, and every record's non-empty locator set, with removal and valid-substitution controls.

## Scope

Resolve AE-P1.2 open item 15 by choosing the lock ownership, authored-state source, ordering, and failure contract that a later G2-B bypass-readiness implementation must obey.

Implementation authorized: `false`.

## Blocked by

- AE-P1.2 item 18: executable acceptance for the ruled two-level readiness model
- A complete chain-edit linearization and hostReady publication-site census

## Review history

- `4a70972ac468d7c1320e95e940b3d4fbcbdd829c`: semantic `BLOCKED`, evidence `BLOCKED`. 978dd9e3 added PASS 3/item 16 supersession, bounded confounder language, stale-offline-waiter coverage, manifest/packet locator resolution, and four new mutations.
- `978dd9e31290551f343b581953c893cf15200c49`: semantic `PASS`, evidence `BLOCKED`. This schema-v2 successor constrains repository paths, reads frozen evidence from the pinned commit, compares governed hashes to pinned blobs and current packet bytes, and adds two structural mutations.
- `1f86e0015f83c666bb2f925eaeb6105a6011b622`: semantic `PASS`, evidence `BLOCKED`. This schema-v3 successor binds the intended external identities, exact governed population, and exact non-empty locator set for every record, with removal and valid-substitution mutations.

## Records

- `G-ITEM15` [gate / READY_FOR_REVIEW]: Item 15 is decided for planning: bypass staging never reacquires controllerMutex; the caller supplies and retains the controller lifetime lock through staging. This packet does not authorize the product change.
- `DEP-PREDECESSOR` [dependency / PINNED]: The predecessor packet commit, tree, and manifest bytes are immutable inputs.
- `DEP-FROZEN-BASE` [dependency / PINNED]: The packet describes exactly the frozen product commit, tree, and governed file bytes.
- `P-GOVERNED-FILES` [population / PINNED]: The governed population is exactly the ten sorted paths in governed_files; each declared digest must match both the immutable frozen-product blob and the packet checkout byte-for-byte.
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
- `CTRL-MUTATIONS` [control / EXECUTABLE]: The checker self-tests semantic controls plus external-identity drift, parent/absolute traversal, self-updated hashes, governed-population removal/substitution, and locator deletion/valid-substitution. All sixteen mutations must be refused.

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
