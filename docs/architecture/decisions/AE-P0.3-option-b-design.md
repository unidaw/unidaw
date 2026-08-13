# AE-P0.3 Option B — RETRACTED

Status: **RETRACTED 2026-08-13**, the day it was written, after independent design
review returned BLOCKED with nine blockers. It is kept rather than deleted because
the reason it was wrong is the ticket's own defect class, and deleting it would
remove the evidence.

**The authority for Option B is, and already was,
`docs/architecture/tasks/AE-P0.3-optionB-review-checklist.md` in the product
checkout `/Users/jak/src/daw`.** Implement against that. Nothing below supersedes it.

## Why this was retracted

**It re-used `B1`–`B8` for different content.** A fully enumerated `B1`–`B8` had
already been issued by an independent reviewer and conditionally passed — twice —
and relayed as a pre-coding requirement. That checklist is dated a day before this
document. This design assigned different content to every one of the eight
identifiers. Anyone approving "B1-B8" would reasonably have believed the issued
conditions were met.

Worst of the eight: the issued `B5` requires an ordered `(quoteId, payloadDigest)`
occurrence digest and states in terms that *an exact count alone cannot catch
same-count replacement, duplicate collapse, or extractor drift*. This document's
`B5` required exactly that count, with `===`, as though it were the strength. It did
not merely omit its predecessor — it asserted the thing the prior review had named
insufficient.

This is the ledger's own recurring shape, a rule restated in two places so one can
rot, except the restatement inverted the rule.

**The cause is not subtle: this was written from the ledger's prose summary of the
Option B decision without ever searching the repository for the issued conditions.**
The ledger paragraph said "acceptance controls B1-B8" and that was taken as a label
to be filled in rather than a document to be found. One `grep` for `optionB` would
have produced the checklist.

**And its single worked example was fabricated.** The record cited
`tools/webstack.sh --dry-run`. That flag does not exist — `webstack.sh` recognises
only `--self-test-free-port` and `--self-test-ready-retirement`, and an unrecognised
first argument falls through and launches the full stack. So the design's only
concrete attestation claimed an observation from an invocation that could not have
produced it. That is precisely the defect AE-P0.3 exists to catch — a document
citing output nobody ran — committed inside the design of the check meant to catch
it, and it would have passed this design's own `B4`, which checks `command` for
presence only.

## What was salvaged

Two things in this document survived review and are recorded here so they are not
re-litigated:

1. **Drop the verbatim-match path.** Measured, not argued: `DEMO.md` contains
   exactly one quoted line today, it is interpolated rather than literal, so the
   verbatim path accepts 0 of 1 current quotes — while admitting comments, usage
   strings and unreachable `--self-test-*` branches out of a 34,272-line corpus of
   which 10,274 lines are full-line comments. Single acceptance path: attestation.
2. **The three open questions are decided.** `attested but unquoted` is not
   legitimate, but the bijection must be over ordered quote OCCURRENCES rather than
   line strings. `reviewer`/`date` stay unauthenticated strings, renamed
   `claimedReviewer`/`claimedObservedAt` with prose naming the boundary — a
   signature inside the same editable file buys nothing; the cost a forger cannot
   pay belongs in the transcript, via raw stream bytes plus blob pins.

## The standing instruction

Do not write a fifth narrowing, and do not write a second design. Implement the
issued checklist, or state explicitly which of its conditions are being asked to be
waived and why.
