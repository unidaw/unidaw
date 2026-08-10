# AE-P0.2 Lane 0 corrective packet

State: `ASSIGNED_BLOCKED_PENDING_ACK`

Implementation owner: `codex-worker-2`
Independent reviewer: `claude-worker-1`
Implementation worktree: `/Users/jak/src/daw-ae-p0-2-corrective`

This packet is immutable after acknowledgement. The owner must acknowledge this
exact packet SHA before edits; any scope change requires a new packet and review.

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

## Exact corrective allowlist

The owner may change exactly these 26 paths, one per line:

1. `docs/architecture/tasks/AE-P0.2-lane0.md`
2. `docs/architecture/tasks/AE-P0.2-ownership.json`
3. `tools/architecture/ae_p0_2/schemas/schema-bundle-identity.schema.json`
4. `tools/architecture/ae_p0_2/schemas/schema-trust-anchor.schema.json`
5. `tools/architecture/ae_p0_2/schemas/ownership-manifest.schema.json`
6. `tools/architecture/ae_p0_2/schemas/ownership-transfer.schema.json`
7. `tools/architecture/ae_p0_2/src/canonical.mjs`
8. `tools/architecture/ae_p0_2/src/generate.mjs`
9. `tools/architecture/ae_p0_2/src/inventory.mjs`
10. `tools/architecture/ae_p0_2/src/bootstrap-validator.mjs`
11. `tools/architecture/ae_p0_2/src/validate-cli.mjs`
12. `tools/architecture/ae_p0_2/bootstrap/schema-trust-anchor.json`
13. `tools/architecture/ae_p0_2/bootstrap/schema-trust-anchor-id.mjs`
14. `tools/architecture/ae_p0_2/generated/schema-bundle-identity.json`
15. `tools/architecture/ae_p0_2/generated/validator.mjs`
16. `tools/architecture/ae_p0_2/generated/contracts.hpp`
17. `tools/architecture/ae_p0_2/generated/contracts.rs`
18. `tools/architecture/ae_p0_2/generated/contracts.ts`
19. `tools/architecture/ae_p0_2/testdata/golden-vectors.json`
20. `tools/architecture/ae_p0_2/tests/bootstrap.test.mjs`
21. `tools/architecture/ae_p0_2/tests/generator-freshness.test.mjs`
22. `tools/architecture/ae_p0_2/tests/ownership-validator.test.mjs`
23. `tools/architecture/ae_p0_2/tests/cross-language.test.mjs`
24. `tools/architecture/ae_p0_2/tests/contracts_cpp_test.cpp`
25. `tools/architecture/ae_p0_2/tests/contracts_rust_test.rs`

The corrective packet itself is governance-only and is not an implementation
path or manifest entry. The final manifest reference set is the baseline set
union the 25 numbered implementation paths; its diagnostic count is 755 and
the canonical path-set comparison—not the count—is authoritative. No other path
may change, be added, deleted, renamed, or replaced. The manifest must include
the corrective packet and all 26 paths.

Add a
negative control for each requirement, independent literal ADR preimage vectors, freshness over
every generated output, exact 755-path reconciliation, and a clean-tree check. No sibling checkout,
 machine cache, product build, or runtime process is permitted. The authorized
 baseline/reference path set must be independently pinned; candidate manifest
 fields may not define their own expected set. Dotfiles such as `.gitignore`
 are valid tracked paths, while absolute, empty, dot/dotdot, NUL, duplicate, or
 unsorted paths are rejected. Review may compile
 only the standalone C++17/Rust 2021 contract fixtures into a unique temporary
 directory; this is explicitly not a product build. TypeScript installation or
type-checking is prohibited in this lane and requires a later accepted
OwnershipTransfer.

Stop if any schema identity cannot be independently recomputed from the ADR, the fixed anchor is
not load-bearing, any wrong-typed/unknown field is accepted, BOM/proxy input passes, generated
language artifacts are placeholders, or any path outside the allowlist changes.

Independent review must recompute the ADR identities and vectors without importing the implementation,
compile the C++/Rust fixtures, exercise every negative control, and return an exact-SHA verdict.
