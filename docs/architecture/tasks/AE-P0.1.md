# AE-P0.1 — Self-contained repository and verification roots

State: `READY FOR ASSIGNMENT`

Implementation owner: `codex-worker-1`

Independent reviewer: `claude-worker-1`

## Outcome

Every build, verification, stack, rehearsal, and browser-e2e entry point in this
ticket resolves files from the checkout that contains that entry point. It never
silently reads source, binaries, dependencies, fixtures, or credentials from
`/Users/jak/src/daw`, `/Users/jak/src/daw-web`, or another sibling checkout.

A repository-integrity guard makes regression executable: tracked absolute
symlinks and user-specific/sibling-checkout references in live build or test
surfaces fail with precise diagnostics. The guard has real negative controls,
not only a scan that happens to pass the current tree.

## Baseline and worktree

- Frozen product baseline: `5bef283798b59c2c4f5720292554c7ab8c265be6`.
- Governance bootstrap ancestor: `762fe34` (`record architecture excellence remediation program`).
- Branch: `ae/p0-roots`.
- Absolute worktree: `/Users/jak/src/daw-ae-p0-roots`.
- The owner must confirm `git merge-base --is-ancestor 5bef283 HEAD` and the
  assigned branch/worktree before editing.

## Dependencies

- Undo baseline freeze is complete.
- All workers acknowledged the canonical path, SHA, and HOLD.
- This is an AE-P0 gate-hardening ticket. No Phase 1 protocol work is authorized.

## Owned files

The implementation owner may edit only:

- `.gitignore`
- `CMakeLists.txt`, solely to register the new integrity test
- `tools/verify.sh`
- `tools/webstack.sh`
- `tools/ask_path_check.sh`
- `tools/demo_rehearsal.sh`
- a new shared repository-root helper under `tools/lib/`
- a new repository-integrity checker and its tests under `tools/`
- `ui-web/test/e2e.mjs`
- the tracked `ui-web/node_modules` symlink, for removal from version control

If a different production or test file must change, stop and request a scope
amendment before editing it.

## Read-only context

- `ARCHITECTURE_EXCELLENCE_PLAN.md`
- `ARCHITECTURE_EXCELLENCE_LEDGER.md`
- `AGENTS.md`
- `ui-web/test/all.mjs`
- `ui-web/test/stack.mjs`
- other scripts under `tools/`, only to compare existing root/resource patterns

Do not edit, clean, install into, build from, or otherwise touch
`/Users/jak/src/daw`, `/Users/jak/src/daw-web`, or `/Users/jak/src/daw-play`.

## Invariants

1. Script/module location, not caller CWD, selects the default repository root.
2. Any supported root override is explicit, canonicalized, validated as the
   expected repository, printed before use, and fails closed on mismatch.
3. Missing local source, binaries, fixtures, dependencies, or credentials never
   trigger a sibling-checkout fallback.
4. `node_modules` is an ignored local directory, never a tracked symlink.
5. The integrity guard inspects tracked repository state deterministically and
   cannot be bypassed by running it from another CWD.
6. The guard distinguishes live executable/configuration surfaces from prose
   with a narrow, documented classification. Do not exclude all Markdown or all
   comments merely to make the baseline pass.
7. No secret value, `.env` content, or credential path is printed by tests.
8. Existing engine, SHM, protocol, and audio behavior is unchanged.

## Required reproducer before implementation

Capture these baseline failures in the handoff:

```sh
cd /Users/jak/src/daw-ae-p0-roots
rg -n '/Users/jak/src/daw-web|/Users/jak/src/daw' \
  tools/verify.sh tools/webstack.sh tools/ask_path_check.sh \
  tools/demo_rehearsal.sh ui-web/test/e2e.mjs
git ls-files -s ui-web/node_modules
readlink ui-web/node_modules
```

The output must show live sibling-checkout references and a tracked absolute
symlink. Add the integrity test first and demonstrate that it fails for those
same reasons before fixing the production surfaces.

## Implementation constraints

- Prefer one auditable root-resolution helper for shell entry points over copied
  path logic.
- JS derives its root from `import.meta.url`/module location, not `process.cwd()`.
- Diagnostics name the offending tracked path and rule.
- The checker and its tests use only repository-standard/runtime dependencies;
  do not add a package for path scanning.
- Temporary negative-control repositories/fixtures must use a validated unique
  temporary directory and clean only that directory.
- Preserve any deliberately supported external binary override only if it is
  explicit and clearly labeled in output. It must not change the source/fixture
  root or become a silent compatibility fallback.
- Do not redesign the whole verification harness, resource namespace, CMake
  dependency model, or web protocol here; those belong to AE-P0.2 and later.

## Required negative controls

Automated tests must prove all of the following:

1. A tracked shell/JS/config fixture containing a user-specific absolute checkout
   path is rejected.
2. A tracked absolute symlink is rejected.
3. An equivalent relative symlink contained inside the fixture repository is
   either accepted by an explicit policy or rejected with a distinct reason.
4. Running the checker and root helper from a foreign CWD still selects the
   assigned worktree.
5. A deliberately poisoned sibling directory cannot affect root resolution or
   verification planning.
6. An invalid explicit override, if overrides remain supported, fails before any
   child process or test runs.

## Acceptance commands

Use worktree-local artifact directories. At minimum run:

```sh
cd /Users/jak/src/daw-ae-p0-roots
git status --short --branch
cmake -S . -B build-ae-p0-roots -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-ae-p0-roots --target daw_lint --parallel 6
ctest --test-dir build-ae-p0-roots -R 'repository.*integrity|root.*isolation' --output-on-failure
bash tools/REPLACE_WITH_FINAL_CHECKER_NAME --self-test
npm ci --prefix ui-web
node --test ui-web/test/unit.mjs
```

Replace the checker placeholder in the handoff with its actual stable command.
Also run the repository scan that proves there are no forbidden live references
and no tracked absolute symlinks. `npm ci` must create only the ignored local
`/Users/jak/src/daw-ae-p0-roots/ui-web/node_modules` directory.

Expected observations:

- Configure/build and every targeted test pass.
- The checker’s negative fixtures fail for the expected rule and its clean
  fixture passes.
- Commands run from `/tmp` still report/use `/Users/jak/src/daw-ae-p0-roots`.
- No command reads or changes a sibling checkout.
- `git status --short` contains only intended source changes before commit and is
  clean after the ticket commit.

## Evidence to return

- Commit SHA and parent SHA.
- Exact changed-file list.
- Baseline reproducer output.
- Each acceptance command, exit status, and concise result.
- Negative-control output naming each triggered rule.
- Resolved source/build/dependency paths observed during verification.
- `git status --short --branch` after commit.
- Any behavior deliberately preserved as an explicit override.

## Review focus

`claude-worker-1` reviews without editing the implementation branch unless the
orchestrator explicitly assigns a repair:

- Try to make a caller CWD or environment variable redirect the scripts.
- Inspect quoting, canonicalization, symlink handling, and path-boundary checks.
- Look for broad allowlists that make the guard cosmetically green.
- Confirm negative fixtures would fail if the implementation regressed.
- Confirm no source/build/test behavior reaches `/Users/jak/src/daw-web`.
- Confirm root CMake changes do nothing beyond registering this test.
- Re-run the targeted acceptance commands in an independently named build dir.

## Stop and escalate

Stop without improvising if:

- the fix requires editing a non-owned file;
- a sibling checkout must be modified, cleaned, or used for dependency install;
- a desired external-root workflow conflicts with the self-contained invariant;
- registering the test requires a broader root-CMake refactor;
- a test needs a secret or paid API call;
- pre-existing changes appear in the assigned worktree; or
- the baseline/branch/worktree does not match this packet.
