# Progress

A running account of what is being worked on and what came out of it. Kept in the repo rather than
in a chat log so it survives the session that produced it.

## Checked facts

<!-- EVERY NUMBER BELOW IS RECOMPUTED BY tools/progress_check.sh, WHICH RUNS IN ctest.
     They are measured against `as-of-commit`, not against your working tree, so you can commit
     freely and catch this file up in one go — but not indefinitely: the check also refuses when
     HEAD has drifted more than a dozen commits past it.
     Run `bash tools/progress_check.sh` and it prints the values to paste. -->

- as-of-commit: 4874fa6
- main-cpp-lines: 6194
- main-function-lines: 5964
- ctest-entries: 169

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
`main()` was ~13,700 lines and the producer lambda held the render path. Eight functions have now
left it, in seven modules — `renderTrack` (1,552), `handleUiEntry` (1,623), `loadProjectFromPath`
(942), `handleAssembledBulk` (444), `saveProjectToPath` (480), `rebuildFlatAndPublish` (133) and
`rebuildAudioRender` (109) together, and `emitChainSnapshot` (143) with `rebuildHostForChain` (121)
— each moved VERBATIM and each verified as such by comparing the moved body line-for-line against
the lambda it came from.

**Two more modules followed** — `engine_track_setup` (setupTrackRuntime + reconcileChildTracks) and
`engine_clip_edit` (locateEditTarget plus the four `apply*` edits) — bringing the total to nine
functions in seven modules.

**Group by the UNION of captures, not one function at a time.** The five clip-edit functions need
3+14+10+10+15 = 52 captures separately and only **23 distinct**: one module and one deps struct
instead of five interfaces onto the same state.

**What actually blocks an extraction is main()'s LINEAR DECLARATION ORDER, not coupling.** A struct
of references cannot be built before its members exist, and these functions are interleaved with the
lambdas they depend on — `findPlacementAt` is declared 300 lines after `applyAddNote` and is needed
by `applyLocalNoteEdit` 50 lines later still. Two remedies keep the move verbatim: split into two
structs constructed at different points, or pass the late dependency as a PARAMETER under the same
name so the body never changes. That constraint, rather than tangled logic, is what makes a single
enormous scope hard to break up: it fixes an order between definitions and uses that nobody chose.

**Order them by CAPTURES, not by size**, and this took four extractions to learn.
`tools/extraction_cost.sh` measures the real cost of a verbatim move: lines are copied unchanged and
so move for free, while every `[&]` capture becomes a struct member, a call-site argument and a
re-binding line. Sorting by line count put `handleAssembledBulk` (446 lines, **11** captures) behind
`loadProjectFromPath` (945 lines, **53**), and `applyAddChord` (118 lines, 15) still outranks the
whole 268-line chain module (4). Every count it prints is a LOWER BOUND — constants need no capture,
so a `constexpr` local the body uses appears only when the new translation unit fails to compile,
which has now happened twice.

**`processTrack` looked like it could not follow them, and that was a wrong decomposition rather
than a hard problem.** Its 29 captures split in two: fourteen are stable main-scope state, but the
other fourteen are declared *inside the producer's per-block loop*, three of them lambdas. Passing
those through a per-block context would construct `std::function` objects per block per track —
heap allocation on the producer path.

Moving the **whole per-block body** instead (1,196 lines, from where `blockId` is claimed to the
bottom of the loop) makes every one of them an ordinary local of the new function.
`processTrack` and `runAudioPatcherNode` come along as locals, no indirection is added anywhere,
and the interface is 51 stable references plus four per-block values as parameters. The unit that
resisted extraction was the wrong unit, not a hard one — worth remembering the next time a piece
looks immovable.

Two things had to be freed first, and neither was about `processTrack`. `EngineAudioCallback`
(1,190 lines) sat in an **anonymous namespace**, so no other translation unit was permitted to name
the type the block body holds a pointer to; it now lives in `apps/engine_audio_callback.h`. And
`kSidechainChannels` had to be *shared* to `apps/engine_types.h` rather than moved, because main()
still uses it.

The split point was already written in main() — "the waits above are deliberately outside it" —
and it was verified rather than trusted: the region was walked tracking brace and loop nesting to
confirm zero `continue`/`break` statements bound to the producer's while loop.

What made each move safe was that the body was relocated VERBATIM — the new function binds local
references carrying the names the lambda captured, so nested lambdas, shadowing and brace scope
still mean what they meant. The only claim the change makes is "the same code, somewhere else",
and for `handleUiEntry` that claim is checked literally: a line-by-line comparison of the moved
body against the pre-move lambda reported 1,623 of 1,623 identical.

**A byte-identical render is weaker evidence than it looks**, and it is worth saying so where the
next person will read it. It is the right oracle for `renderTrack`, which is the render path. For
`handleUiEntry` the render never enters the code that moved, so an identical hash there only says
the audio path was left alone. The suite is what actually exercises a UI dispatcher.

**The capture enumeration has a blind spot.** Emptying the `[&]` and compiling with
`-ferror-limit=0` lists what must be passed — but constants are exempt from capture, so a
`constexpr` local used by the body is not on the list and shows up only when the new translation
unit fails to compile. Two did.

Three things had to be freed first, and they explain the shape of the problem: `WorkerPool`,
`dispatchRustKernel` and `getClipEventsInRange` were all declared inside `daw_engine_main.cpp`,
so nothing else could name them. `buildClipSnapshot` was the fourth, found the same way — the
extraction fails to compile, because a file-scope helper is not a capture and no amount of capture
analysis will mention it. A helper written beside its only caller is invisible everywhere
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

## Testability, which is the axis the extraction was supposed to buy

The panel's other complaint was that **99% of the suite's runtime boots processes**. Extracting a
function only *enables* a unit test; it does not write one, and an extraction with no test behind it
has improved a line count and nothing else. Two are now written, both against rules that could
previously only be reached through an engine, a shared-memory ring and a typed note:

- **`rebuildFlatAndPublish`'s mute prune** — a mute naming a deleted note is dropped, but ONLY when
  the clip is installed, because during load a placement can name a clip that has not arrived yet
  and every mute on it would look dead. Deleting the guard outright segfaults; the interesting
  sabotage is the plausible one (treat an absent clip as proof the mutes are dead), which fails
  exactly the assertion written for it.
- **`locateEditTarget`'s two rules** — a remove landing outside every placement mints nothing, and
  a new clip inherits the predecessor's grid instead of snapping back to 4/4.

A third covers **`handleAssembledBulk`'s size checks** — an envelope declaring more points than it
carries is refused rather than applied short, because half an envelope is a *valid* envelope and
delivering it produces a wrong sound instead of an error.

All three were verified by sabotage rather than by passing. The pattern worth keeping: pick the rule
whose failure is QUIET. A crash gets found; a delete that silently creates a clip, a section whose
rows are subdivided differently than the one before it, or an instrument that sounds slightly wrong,
gets blamed on the user.

**And two of the three first drafts tested nothing.** The bulk one asserted a COUNT — "four envelope
points are held after applying four" — which passed while the engine was logging
`sampler.envelope_rejected` on the same line, because applying an envelope mints a default ADSR that
also has four points, and my fixture had built a command the engine correctly refused. The tell was
in the run's own output rather than in the assertion. Assert the VALUES.

## One thing that did NOT work, recorded so it is not tried again

Two checks broke during the extraction — `op_registry_check` and `hazard_order_check` — each because
it derived a fact from `apps/daw_engine_main.cpp` **by name** and the code had moved. Both failed
loudly only because someone had written an explicit "the derivation found NOTHING" guard into them.
That is a twice-observed pattern, so I tried to automate it: an audit that stubs the engine's source
and reports any check that still passes.

**It does not generalise, and four attempts each failed differently.** Stubbing only `main.cpp` flags
every check that correctly reads the new modules. Stubbing the module `.cpp` files flags every check
that correctly reads a header — `hazard_order_check` reads `engine_audio_callback.h`. Stubbing all
176 files in `apps/` finally has no *source*-level false positives, and then flags four checks that
run the built ENGINE and never read source at all. Every accusation the tool made was wrong.

The reason is that "the subject of a check" is check-specific: source, header, another tree, or a
running binary. A generic stub cannot tell "reads nothing" from "reads somewhere else". So the guard
has to be written per check, which is exactly what caught both moves. The tool was deleted rather
than shipped — a check-auditing tool that produces confident false accusations would send someone to
"fix" four healthy checks, which is worse than not having it.

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
  thread behind an in-order bounded request queue. The load harness now exists
  (`tools/rt_load_probe.sh`) and it answers the question it was built for: under 3.7x-4.2x measured
  contention, realtime and QoS are indistinguishable **because the producer uses about one percent
  of its block budget**. With that much headroom no scheduling policy can matter, so the tie is a
  fact about the workload rather than about the scheduler. What #55 needs is a session heavy enough
  to fill the budget — real plugins or many sampler tracks — not more instrumentation.

  I first recorded the opposite here, that the engine could not measure render cost at all. It
  always could: `producer.load` reports mean and peak block cost against the budget, with a sampler
  share and pool-engagement hysteresis. I had grepped for the wrong words and was one commit from
  writing a worse second copy of it.

**Both of the first two are now MEASURED even though neither is fixed**, by inverted checks that
assert the defect is present and announce their own retirement:
`tools/pdc_window_check.sh` and `tools/meter_publish_check.sh`. Each was verified by simulating
its fix and confirming the check reports "this has been fixed, delete this block" rather than
reading as a regression. So neither defect can drift while the decision is outstanding, and
whoever lands a fix gets told what to do with the check that was holding the line.
