# AE-P0.1 repair packet — current-main checkout isolation

State: `ASSIGNED`

Product baseline: `62bafdc6cf1cd53168ce73d098cd6acc78659be8`

Owner: `codex-worker-1`

Independent reviewer: `claude-worker-1`

Parent evidence: historical AE-P0.1 review of `d72230611885852e5eca201c2e427c6cc0567f89`

Worktree: `/Users/jak/src/daw-ae-p0-roots-current`

## Objective

Repair the checkout-local verification-root guard and web-stack isolation on
the current baseline. The implementation must make source/build/runtime
selection attributable to the invoking checkout, fail closed on steering, and
keep credential and dependency authority local to the run. Do not redesign the
engine, SHM protocol, plugin architecture, or unrelated frontend behavior.

## Required fixes from independent review

1. Fix `tools/webstack.sh` free-port startup: a normal `lsof` result of “no
   listener” must not abort under `set -euo pipefail`. Add a control that starts
   the stack with a genuinely free page port, without running the physical
   device suite in parallel.
2. Scan both tracked/index representation and the actual working-tree
   representation used by verification. A dirty forbidden path must not
   false-pass merely because its index OID is clean; ordinary permitted dirty
   development remains supported.
3. Expand the live-file guard to cover operational Markdown actually consumed by
   registered checks, and close sibling/home/path-expression bypass forms. Do
   not use a dead or broad extension allowlist. Preserve explicit provenance for
   every excluded file class.
4. Add a mode-100644, no-shebang `.sh` fixture and assert the specific extension
   rule fired. An aggregate self-test count is insufficient.
5. Change the tracked ignore rule so a `node_modules` symlink name cannot be
   re-added; preserve the guard that rejects any tracked absolute dependency
   symlink.
6. Make credential-free stack execution the default. Paid/credentialed mode
   must be explicit at the call site, and must not discover checkout or home
   `.env` files accidentally. The no-credential path must still work in
   worktrees containing `.env`, using run-owned resource paths.
7. Reject legacy `ENGINE`/`HOST` overrides instead of silently ignoring them.
8. Add the packet-SHA/coverage controls necessary to prevent reviewer and
   implementer drift from later task amendments; keep changes narrowly scoped.

## Ownership and forbidden scope

Owned files are limited to the checkout-root verification helpers, their direct
self-tests, the web-stack helper/test harness, `.gitignore`, and this packet.
Do not edit C++ engine sources, `platform_juce`, SHM/protocol files, Rust bridge
protocol definitions, generated artifacts, or unrelated UI behavior. Do not
modify the historical worktree `/Users/jak/src/daw-ae-p0-roots` or commit into
`main`.

## Verification

Before handoff, run only in this worktree and record exact commands/results:

- repository-root guard from at least four CWDs and environment steering cases;
- guard self-test including the non-executable `.sh` fixture;
- focused shell/unit tests for free-port startup and credential isolation;
- relevant web unit tests without installing through tracked symlinks;
- isolated CTest repository-integrity/root-isolation checks if available;
- `git diff --check` and a clean worktree after commit.

Do not run the full device-owning CTest/web/e2e suites. Do not use
`watch-next.mjs`. Do not start work until the orchestrator has delivered this
exact packet SHA and the worker acknowledges it.

## Delivery

Commit the repair on `ae/p0-roots-current`, report the exact commit, packet SHA,
commands/results, changed-path list, and any residual risk. The reviewer must
review that exact commit against this exact packet before integration.
