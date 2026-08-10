# AE-P0.2 Lane 0 corrective packet

State: `DRAFT_FOR_REVIEW`

This packet reopens Lane 0 against the reviewed baseline `89d0f6cfddb6350c15d239f13058221d5e1b6051`.
It is additive/corrective only; no product runtime, protocol hotspot, or client behavior changes.

## Required closures

1. Compute every schema identity from the ADR schema preimage
   `SHA-256("daw-schema-v1\\0" || JCS(schema_without_self_id))`; store schemas as canonical
   JCS bytes and record both schema ID and canonical-byte digest.
2. The independently owned fixed trust-anchor ID must be imported and compared to the candidate
   anchor before bundle/document/generated validation is trusted.
3. Validate all four documents against closed typed schemas (`additionalProperties:false`) at
   every nested boundary; reject missing, unknown, and wrong-typed fields.
4. Reject a raw UTF-8 BOM before decoding; reject proxies/accessors/non-plain objects and other
   non-deterministic canonicalization inputs.
5. Generated C++/Rust/TypeScript outputs must contain the declared types, validators, canonical
   writers, and literal vectors. C++17/Rust 2021 tests compile and execute in the lane. TypeScript
   checking is deferred only through an explicit accepted OwnershipTransfer.

## Scope and controls

Owner may change only the existing Lane 0 generated/source/test paths plus this packet. Add a
negative control for each requirement, independent literal ADR preimage vectors, freshness over
every generated output, exact 755-path reconciliation, and a clean-tree check. No sibling checkout,
machine cache, product build, or runtime process is permitted.

Stop if any schema identity cannot be independently recomputed from the ADR, the fixed anchor is
not load-bearing, any wrong-typed/unknown field is accepted, BOM/proxy input passes, generated
language artifacts are placeholders, or any path outside the allowlist changes.

Independent review must recompute the ADR identities and vectors without importing the implementation,
compile the C++/Rust fixtures, exercise every negative control, and return an exact-SHA verdict.
