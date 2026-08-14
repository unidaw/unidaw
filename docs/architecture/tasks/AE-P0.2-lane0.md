# AE-P0.2 Lane 0 packet — bootstrap contracts and ownership authority

State: `ASSIGNED — IMPLEMENTATION BLOCKED PENDING PACKET-SHA ACK`

Product baseline: `c33da66fe1a66f20eee931335b18465cfddfdb0e`

Approved ADR: `7dff997ec78824e821979d056de45840b2135322`

Owner: `codex-worker-2`

Independent reviewer: `claude-worker-2` or another backend-designated Claude
reviewer

Branch: `ae/p0-2-lane0`

Worktree: the sibling checkout `daw-ae-p0-2-lane0` (recorded without its absolute path: an
absolute home-directory path in live content is what `repository_integrity` refuses, and this
line was invisible to it only because nothing classified this document until 2026-08-14)

## Objective

Create only the Lane 0 bootstrap needed to make later AE-P0.2 ownership and
contract work enforceable: the closed ownership schemas, their one generator,
generated cross-language validators and canonical writers, the independently
anchored bootstrap validator, literal golden vectors, focused standalone tests,
and the exact repository ownership manifest.

This packet does not authorize product or migration implementation. It creates
the authority that later lanes must satisfy.

## Packet acknowledgement gate

This packet file is the first and only changed path in the packet commit. After
committing it, the owner reports the exact commit SHA to `backend` and stops.
No other path below may be created and no implementation command may run until
`backend` acknowledges that exact packet SHA.

The packet is immutable once acknowledged. Any scope amendment requires a new
packet commit, a new exact-SHA acknowledgement, and independent review.

## Exact additive ownership

Lane 0 may add exactly these 25 paths and may not edit, delete, rename, or
replace any existing path:

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

All paths are additive and isolated. No root `CMakeLists.txt`, product source,
protocol/SHM file, generated product artifact, existing verification entry
point, ledger, existing task packet, dependency manifest, or unrelated
documentation is owned by Lane 0.

## Contract boundaries

- The four schema sources use JSON Schema 2020-12, reject unknown properties at
  every object boundary, use closed enums/tagged unions, and define canonical
  ordering and duplicate rejection.
- Canonical JSON is RFC 8785 JCS and every identity digest uses SHA-256 with the
  ADR's exact domain-separated byte preimage. No implementation-local meaning
  of "canonical" is permitted.
- `src/generate.mjs` is the sole generator. It emits the schema bundle,
  standalone generated validator, and C++/Rust/TypeScript contracts containing
  types, validators, canonical writers, and literal-vector constants.
- `src/bootstrap-validator.mjs` is independent of the generated validator. The
  fixed trust-anchor ID constant is outside `generated/` and must match the
  hand-reviewed anchor bytes before generated parsing is trusted.
- `src/validate-cli.mjs` is a new standalone reader/validator. Wiring it into an
  existing verification entry point belongs to a later owned lane.
- Tests are standalone new files. They may invoke installed Node, C++, and Rust
  tools directly, but may not change root build/package integration. The
  generated TypeScript contract remains a Lane 0 deliverable, but TypeScript
  type-checking moves to the later web/toolchain-owned lane and must be added by
  an accepted OwnershipTransfer before that lane begins; Lane 0 may not install
  a TypeScript compiler or silently skip a nominal TypeScript check.

## Ownership manifest rules

`docs/architecture/tasks/AE-P0.2-ownership.json` is generated from an explicit
reference set: the exact tree tracked at baseline
`c33da66fe1a66f20eee931335b18465cfddfdb0e` unioned with the exact 25-path
planned list above. It must:

1. enumerate all 730 paths tracked at product baseline
   `c33da66fe1a66f20eee931335b18465cfddfdb0e` and all 25
   planned Lane 0 paths as exact, canonical, unique entries;
2. classify `existing` versus `planned` solely against the tracked tree at
   `c33da66fe1a66f20eee931335b18465cfddfdb0e`: a baseline path is `existing`,
   while every approved path absent from that baseline remains `planned` even
   after a later commit creates it; specifically,
   `docs/architecture/tasks/AE-P0.2-lane0.md` remains `planned` although packet
   commit `4dbc387abb679d9db8647c6ce627eb44b2679934` already tracks it;
3. assign exactly one owner, one independent review owner, one dependency lane,
   and one transfer state to every entry;
4. mark out-of-scope baseline files frozen rather than silently unowned;
5. assign all Lane 0 paths to `codex-worker-2`, independent Claude review, and
   the Lane 0 bootstrap dependency;
6. reject a missing tracked path, an unknown extra path, a duplicate/alias path,
   multiple owners, a missing reviewer/dependency/transfer field, or a glob/count
   used as authority; and
7. require an accepted `OwnershipTransfer` before any later lane changes an
   owner or creates a path absent from this manifest.

The generator may use deterministic classification rules to prepare entries,
but only the emitted exact path entries are authority. Counts and globs are
diagnostics. Reconciliation compares the manifest's canonical path set with the
explicit baseline-union-planned reference set; it never derives or changes the
state field from `HEAD`. During Lane 0 construction the tracked `HEAD` set must
be a subset of that reference set with every baseline path present, and the
final Lane 0 implementation commit must equal the full reference set.

## Verification after packet acknowledgement

After `backend` acknowledges the packet SHA, Lane 0 may run only focused,
non-product verification:

- generator freshness and byte-for-byte regeneration;
- bootstrap stale-anchor/schema/validator substitution controls;
- ownership schema positive and negative vectors;
- C++/Rust literal-vector and validator smoke tests using only the new
  standalone files; TypeScript type-checking is a mandatory later
  toolchain-owned-lane gate, not a skipped Lane 0 check;
- exact baseline/planned path-set reconciliation;
- `git diff --check`, changed-path allowlist checks, and clean committed state.

Do not build or run the DAW, open the physical audio device, run CTest/Rust e2e/
web product suites, install dependencies, use network access, or modify another
worktree.

## Delivery and review

The implementation handoff must include the acknowledged packet SHA, exact
implementation commit, changed-path list, generator/verification commands and
results, and residual risks. A backend-designated Claude reviewer must review
that exact implementation commit against this exact packet before integration.
