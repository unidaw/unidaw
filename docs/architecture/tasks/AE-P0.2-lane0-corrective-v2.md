# AE-P0.2 Lane 0 corrective packet — baseline amendment

State: `ASSIGNED_BLOCKED_PENDING_ACK`

This packet supersedes corrective packet
`d9df54fa28cdf8c1235009547682d3679eade83b` because the authoritative product
baseline was amended by the Undo owner. It retains that packet's exact scope,
closures, controls, and review obligations without weakening them.

## Updated authority

Implementation must be based on product `main` at exact commit
`7710401` (full SHA must be recorded by the owner before edits). This commit
contains the completed Undo baseline and supersedes the previously pinned
`89d0f6cfddb6350c15d239f13058221d5e1b6051`. The implementation worktree is
`/Users/jak/src/daw-ae-p0-2-corrective`; it must be reset/recreated only by
preserving any uncommitted authorized schema patch and then checked clean
before new edits. Product main and unrelated user changes remain untouched.

The owner, reviewer, exact 25-path allowlist, schema closures, trust-anchor
requirements, generated-output behavioral parity, malformed-input controls,
independent reference-set pin, scoped fixture compilation permission, and
TypeScript prohibition are exactly those in packet `d9df54f`. No other scope is
authorized. This amendment is immutable after owner and reviewer ACK; any
further baseline or scope change requires another packet and review.

## Required acknowledgements

The implementation owner must ACK this exact packet SHA and report the full
`7710401` SHA plus a clean worktree/base check before edits. The designated
independent reviewer must approve this exact packet SHA. Until both are recorded,
the corrective lane is paused.
