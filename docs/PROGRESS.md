# Progress

A running account of what is being worked on and what came out of it. Kept in the repo rather than
in a chat log so it survives the session that produced it.

## Checked facts

<!-- EVERY NUMBER BELOW IS RECOMPUTED BY tools/progress_check.sh, WHICH RUNS IN ctest.
     They are measured against `as-of-commit`, not against your working tree, so you can commit
     freely and catch this file up in one go — but not indefinitely: the check also refuses when
     HEAD has drifted more than a dozen commits past it.
     Run `bash tools/progress_check.sh` and it prints the values to paste. -->

- as-of-commit: 1d039a2
- main-cpp-lines: 13138
- main-function-lines: 11666
- ctest-entries: 165

## Why this file cannot quietly go stale

Because it is not maintained by remembering. Three separate mechanisms, none of which is a habit:

1. **The numbers are recomputed.** `tools/progress_check.sh` reads `as-of-commit`, checks out those
   three facts from *that commit's* tree, and fails if any of them disagrees with what is written
   here. Wrong numbers are a red suite, not a stale document.
2. **Drift is bounded.** `as-of-commit` must be an ancestor of HEAD and within `MAX_DRIFT` commits
   of it. Stop updating and the suite goes red on its own, without anyone noticing first.
3. **Citations are already checked.** This file lives in `docs/`, so `tools/doc_citation_check.sh`
   applies to it: no line-number citations, and every source file and symbol it names must
   actually exist. That is the rule that catches the actual failure mode — a claim about the code
   that stopped being true.

   That rule caught this very paragraph within a minute of it being written: the sentence
   originally spelled out a placeholder path to illustrate the format, and the check flagged it,
   because a check reading prose cannot tell a mention from a use. The fix is to stop naming an
   example rather than to add an exemption — a rule that has to dodge its own text is telling you
   something about its precision, and an exemption would be the first hole in it.

**What none of this can do is make the prose useful.** A check can force the numbers to be honest
and the file to be recent; it cannot force the narrative to say anything worth reading. That part
is a judgement call every time, and pretending otherwise would be the same mistake as a comment
that claims to be a guarantee.

The reason it is built this way: every stale claim found in this repo was in a file somebody
*intended* to keep current. `tools/meter_bar_check.sh` carried "THE ANCHOR IS NOT FIXED" for months
after it was fixed — stale in the direction that gets work done twice, by someone with no reason to
suspect the first attempt exists.

## 2026-08-02/03 — engine refactor and the bugs it surfaced

`apps/daw_engine_main.cpp` started this work at **20,387 lines** with `main()` at **13,652** and
the suite at **149** entries. Where all three stand now is in the checked-facts block above and
nowhere else in this file — those numbers move every few commits, and a second copy in prose is a
copy that drifts. This one did: it said `main()` was 12,133 while the tree said 11,666, within
hours of being written. 426 unit assertions now exist in modules that previously had none.

**`main()`'s own length is what the panel graded**, and most of the file's earlier shrinkage did
not touch it — that came from merging duplicated rules. It only started moving when `renderTrack`
(1,552 lines, plus `emitNotes` nested inside it) left for `apps/engine_render_track.cpp`, followed
by `saveProjectToPath`.

**Structure.** 16 command modules extracted from `handleUiEntry` (5,604 → ~1,530 lines);
`apps/engine_types.h` for 25 types that had been declared *inside* `main()`; `apps/engine_pure.h`
and `apps/engine_rt_helpers.h` for rules that can now be tested without booting a process. Four
unit-test binaries where there were none. A command module rebuilds in 4.2s against 13.5s to
relink the engine.

**The grade was C, and the binding constraint was `main()`.** A four-judge panel put it plainly:
`main()` was ~13,700 lines and the producer lambda held the render path. `renderTrack` is now out;
`processTrack` (796), `handleUiEntry` (1,625), `loadProjectFromPath` (945) and `saveProjectToPath`
(480) remain.

What made the move safe was that the body was relocated VERBATIM — the new function binds local
references carrying the names the lambda captured, so nested lambdas, shadowing and brace scope
still mean what they meant. The only claim the change makes is "the same code, somewhere else",
and a byte-identical render checks exactly that claim.

Three things had to be freed first, and they explain the shape of the problem: `WorkerPool`,
`dispatchRustKernel` and `getClipEventsInRange` were all declared inside `daw_engine_main.cpp`,
so nothing else could name them. A helper written beside its only caller is invisible everywhere
else, so the next caller gets written beside it too — that is how a function reaches 15,000 lines,
one reasonable-looking dependency at a time.

**A 6-line duplicate-block scan over `main.cpp` has yet to find a duplication that was actually
identical**, which is why it keeps producing bugs rather than tidying:

- **Placement reach** was answered in five places, three different ways. Two guards each existed at
  exactly *one* site, and each one's comment documented the bug that motivated it: a freshly
  created clip's placement measured as *empty* everywhere except the published extent, and a
  saturating add that three copies lacked — one of which feeds the ripple-refusal decision, where
  a wrapped end silently accepts or refuses a time edit.
- **A device with no `capability_mask` loaded as a device with no capabilities.** An absent field
  defaulted to 0, so the device loaded without complaint, appeared in the chain, and was inert.
  Only hand-authored projects were affected — which is most of the fixtures here.
- **The hazard-pointer acquire** existed in three verbatim copies of a lock-free protocol, with a
  comment defending the duplication that said "both sites" while there were three.
- **The loop rules** are where duplication was *kept*: a seek clamps where the transport wraps, so
  they stayed three named functions whose tests assert they disagree.

**Two instrument failures cost more than any of the bugs.** `sampler_vintage` failed the engine for
being correct — its assertion was a proxy that only held when the sample-and-hold period was not a
whole number of frames, and connecting Bluetooth headphones moved the default output device from
44.1k to 48k. That led to `--sample-rate` on the offline render (a bounce previously took whatever
was plugged in), which in turn exposed a second copy of the sample rate: `effSampleRate` read the
device directly while `baseConfig` carried its own — the same config-versus-pump divergence
`--block-size` had already been repaired for, one field away.

## Open, and needing a decision rather than work

- **Published meter points carry the requested tick, not the effective one.** `setMap` snaps a
  signature change forward to the next bar line; the wire publishes the tick that was *asked for*.
  Measured: 3/4 requested at 5 quarters, published as 5, actually begins at 8. Fixing it properly
  means widening `UiTimeSigPoint` and bumping `kShmVersion`, which the equality gate turns into a
  forced rebuild on both sides — so it needs coordination, not just code.
- **PDC clamps every event in the first `latencySamples_` to sample 0**, collapsing their relative
  timing. Three candidate fixes, and choosing among them is an owner's call.
- **CoreAudio workgroups for the host render thread.** Cross-process `os_workgroup` sharing needs
  raw mach ports — JUCE 8's `AudioIODevice::getWorkgroup()` / `AudioWorkgroup::join()` hide the
  handle — plus a dedicated RT render thread in the host, split off the control/instantiation
  thread behind an in-order bounded request queue. Large, and its benefit is only visible under
  contention: realtime scheduling and QoS measured as a TIE on an idle machine, so "did it help"
  cannot be answered without a load harness that does not exist yet.

**Both of the first two are now MEASURED even though neither is fixed**, by inverted checks that
assert the defect is present and announce their own retirement:
`tools/pdc_window_check.sh` and `tools/meter_publish_check.sh`. Each was verified by simulating
its fix and confirming the check reports "this has been fixed, delete this block" rather than
reading as a regression. So neither defect can drift while the decision is outstanding, and
whoever lands a fix gets told what to do with the check that was holding the line.
