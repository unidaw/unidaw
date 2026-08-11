# Architecture review convergence protocol

This protocol preserves adversarial quality while preventing serial prose-repair loops. It applies to every AE phase packet.

## One source of truth

Each packet MUST have a machine-readable gate manifest with one record per gate, population, ruling, dependency, count, control, and implementation decision. Every record has a stable ID, owner, status, dependencies, exact source span, and executable control (or an explicit `WITHDRAWN` reason).

Packet prose, summaries, counts, dependency diagrams, and self-checks MUST be generated from or mechanically checked against that manifest. Repeated hand-written counts are forbidden. A gate cannot be PASS if its manifest record, population, dependencies, or controls are absent or contradictory.

## Review loop

1. Owner publishes an immutable packet SHA and manifest SHA.
2. Semantic reviewer checks lineage, scope, parser bijection, counts, ruling propagation, dependency graph, negative controls, and clean tree.
3. Evidence verifier uses one disposable checkout pinned to that SHA, reads files once, caches excerpts, and records machine-readable evidence. No repeated `git show`, broad archaeology, product builds, runtime execution, or edits.
4. Planner produces implementation tickets only from the reviewed manifest.
5. Lead integrates only after independent semantic and evidence `PASS` for the same packet SHA and frozen product base.

Reviews run in parallel. A reviewer returns `PASS` or a numbered blocker list.

## Structural-fix rule

If the same defect class fails twice, sentence edits are prohibited. The next packet MUST add a schema field, parser invariant, generated output, explicit delimiter, or negative control that makes the defect mechanically impossible.

## Release-candidate rule

After exact semantic and evidence `PASS`, the packet becomes a release candidate. Further improvements are follow-up tickets unless they invalidate a manifest invariant. A successor must name its predecessor SHA, changed records, and reopening reason.

No implementation, version bump, or product build is authorized while the packet is `DRAFT`, `CHANGES_REQUESTED`, or `BLOCKED`.
