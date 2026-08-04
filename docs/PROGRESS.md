# Progress

A running account of what is being worked on and what came out of it. Kept in the repo rather than
in a chat log so it survives the session that produced it.

## Checked facts

<!-- EVERY NUMBER BELOW IS RECOMPUTED BY tools/progress_check.sh, WHICH RUNS IN ctest.
     They are measured against `as-of-commit`, not against your working tree, so you can commit
     freely and catch this file up in one go — but not indefinitely: the check also refuses when
     HEAD has drifted more than a dozen commits past it.
     Run `bash tools/progress_check.sh` and it prints the values to paste. -->

- as-of-commit: f1b0352
- main-cpp-lines: 3909
- main-function-lines: 3674
- ctest-entries: 176
- main-function-ceiling: 3232

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

**The consumer thread followed**, which is everything the engine publishes outward — track state,
clip extents, arrangement summary, automation lanes, patcher graph, harmony, plus the aux-child
reconcile and host-restart scheduling decided from the same snapshot. 719 lines, 43 dependencies.
`main()` is now 5,256 lines against 13,652 when this started.

**Where the render oracle stops applying.** It gated most of this sweep and it is the right check
for `renderTrack` and the per-block body. It says almost nothing about the dispatcher, project load,
or this consumer: none of them touch audio, so a byte-identical render only reports that the render
path was left alone. What covers those is the line-for-line body diff plus the suite's UI checks,
which read the very shared-memory blocks the consumer writes.

**And the six UI writers followed the consumer**, since it was their only caller — six
single-caller lambdas left behind in a file their caller had left is not a shape worth keeping. They
get their own `UiWriterDeps` (27 members) nested inside `ConsumerDeps` rather than swelling it to
59, which is what the dispatcher already does with its sixteen `*CommandDeps`.

**`main()` is 4,909 lines against 13,652 when this started, and `main.cpp` 5,140 against 20,387.**

## 2026-08-04 — the re-score, and what a panel found that the suite did not

The refactor above was done against a **C** from a four-judge panel. It was never re-scored, so
"the objections are addressed" was a claim about my own work, resting on the items that panel named
rather than on anything measured. Four judges read the tree again — read-only, and told explicitly
to verify numbers themselves rather than trust this file, because this file is written by the same
party whose work is being graded.

**C (structure) / C (change cost) / B− (tests) / D (correctness).** Not the B that was asked for.

**The structure verdict is the one that matters, and it is correct.** A 1,625-line lambda became a
1,700-line function. Relocation is not decomposition. Seven modules now exist whose largest function
is bigger than most codebases' largest *file*, and no further amount of verbatim moving changes
that — the next increment has to cut bodies, not carry them. Measured: 1,308 of `handleUiEntry`'s
1,746 lines sat in dispatch arms that call no handler at all.

**Two live product bugs, neither of which any check could see.**

- *A patcher played the wrong track's instrument.* The pool is global — every track's subgraph
  concatenated — and ownership is enforced only at render time, by asking whether **this track** has
  a patcher device. A track with none did not get a smaller set; it disabled the ownership guard
  entirely and ran every node in the project, merging their events into its own instrument. Measured
  on two tracks: track 1, holding no patcher and no note, sounded track 0's generated notes at its
  own sample's pitch while track 0 stayed silent. The engine already knew the right answer
  (`patcherAssembledFromDevices`); `RenderTrackDeps` simply did not carry it, so the render path
  guessed from the only thing it could see.
- *The web UI's overlap indicator read the sound-addressed bit.* `FLAG_ALLOW_OVERLAP` was 8 with a
  comment saying "Bit 4"; the engine's bit 4 is 16 and its bit 3 is `kUiMixFlagSoundAddressed`. Rust
  had it right. One of three mirrors diverged, and it was the one mirror with no check on it.

**Both existing checks that looked like they covered the patcher case were single-track fixtures.**
A one-track project cannot express "the other track". That is the same shape recorded elsewhere in
this file for the kit read-back: a fixture that cannot represent the defect is not coverage.

**The test judge's finding was not "too few tests".** It was that the mechanism deciding *which*
tests run was broken. `check_registry_check.sh` asked whether a check's filename appeared as a
substring of CMakeLists — and `readback_check.sh` is a suffix of four registered
`*_readback_check.sh` entries. So the one check in the tree that had never executed was reported by
the ratchet as registered, while three separate commits improved it. Its header already described
this failure twice, in other directions, and fixed each by tightening the match. The rule is now a
function with a self-test that runs every invocation, and the tree's verdict is suppressed when the
rule fails its own cases.

**`audio_stability_check` was passing on two idle runs.** Every assertion it made compared the deep
pipeline against the shallow one, and all of them are satisfied by the shallow run reporting zero —
which is the load not biting, not the lever working. It never asserted the load bit. At its own
calibration the result alternates between real and vacuous run to run, reporting both identically.
It now exits 77 and ctest prints SKIP, the first entry in the suite able to say "I could not
answer".

**What this round says about ratchets generally.** Four of them fired on each other's work during
it: `doc_citation_check` caught the new self-test's placeholder names, then caught the comment
explaining that fix; `progress_check` caught this file; `-Werror=unused-variable` named the fourteen
dependencies that an extraction had orphaned. Ratchets disagreeing with each other's text, and
being right, is the system working — and the fourteen orphaned deps are the difference between
decomposition and relocation, since silencing them would have left the hub struct at 73 members.

## 2026-08-04 (later) — the wall, and engine objects instead

The re-score above named main() as the binding constraint. Four thread bodies came out of it
(xrunReporter, restartWorker, uiThread, masterRenderThread) at 5-6 dependencies each, and
applyRemoveNote came out with a 39-line duplicate of engine_clip_edit deleted rather than moved.
Then the approach stopped working, and the measurement is why:

    the producer thread          310 lines  ->  a 53-member deps struct
    the 14 largest lambdas       699 lines  ->  a 116-member deps struct
    per group (undo, harmony, track, modlink)  ~19-30 new members for ~100 lines each

The union amortises exactly as the group-by-captures rule predicts, and plateaus near SIX LINES OF
BODY PER NEW DEPENDENCY. Both figures are worse than the 72-member HandleUiEntryDeps the panel named
as a defect, so continuing would trade the structure axis against the coupling axis — which is
precisely what the re-score caught happening the first time round (deps 466 -> 486 while shrinkage
was being reported). The producer version was built, measured and REVERTED.

**These lambdas are not handlers that happen to live in main(). They ARE main()'s state
manipulation**, and a struct of references cannot express that more cheaply than the state does.

**So the state moves with the code that owns it.** Two objects so far:

  HarmonyTimeline   4 state members + 4 operations. Those four appeared as 17 separate members
                    across seven *Deps structs; each now holds one HarmonyTimeline&.
  TransportState    6 atomics, no methods. They appeared 25 times across ten structs.

    total deps members   509 -> 485, past the 486 the re-score measured
    main()             4,922 -> 4,312

**Cohesive objects, NOT one god EngineState** — that would be the 72-member hub again at larger
scale. And the transport deliberately has no methods: the transport COMMANDS are a separate module,
because a command validates and journals and refuses on the UI thread while this holds six numbers
the producer reads every block.

**Member names are the old capture names on purpose** — `harmonyEvents`, not `events`. That is what
lets a 95-line move be proven identical by a line-for-line diff instead of reviewed by eye. Renaming
is a separate edit.

**main-function-ceiling** is a monotone ratchet added the same day, measured on the WORKING TREE
with `<=`. The equality check that preceded it green-lit a 13-line regression in the very number a
panel had named as the constraint, because a fact that is checked is not a constraint that is held.
It has been lowered eight times since and raised never — and it has already refused two of my own
commits, once when a comment I added pushed main() one line over and I compacted the code instead.

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

A fourth covers **`reconcileChildTracks`** — which aux child tracks a multi-out parent should have.
The consumer calls it on EVERY tick, so its idempotence is load-bearing: a broken "does this child
exist" test would append a child per tick until the 64-track budget broke. It also pins the sampler
stem-to-bus synthesis, which is the gap S6 left (a sampler has no host to ask for a bus layout, so a
sampler-only multi-out track used to get no children at all).

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
  thread behind an in-order bounded request queue.

  **The measurement question is settled, which was the actual blocker.** `tools/rt_load_probe.sh`
  now runs the heavy sampler fixture (24 tracks, dense 16ths, 64 voices, the expensive
  interpolator) instead of the fake identity instrument, which moves the producer from ~1% of its
  block budget to about a third. Four interleaved rounds under 3.5x-3.8x measured contention:

      realtime, loaded    producer 31.5% .. 33.1% of budget
      QoS only,  loaded   producer 38.4% .. 41.4% of budget

  Non-overlapping. Without the mach time-constraint promotion the producer is descheduled
  mid-block and the wall-clock cost of a block rises about a fifth — so realtime scheduling is
  earning its keep, and a workgroup change should be judged the same way.

  **Underruns do NOT separate** (0.1%-1.0% against 0.0%-1.9%, overlapping): at a third of budget
  there is headroom to absorb the difference, so the cost moved and the dropouts did not. Judging
  this work on dropout counts would read noise; judging it on a tie would have missed the effect.

  I first recorded here that the engine could not measure render cost at all. It always could:
  `producer.load` reports mean and peak block cost against the budget. I had grepped for the wrong
  words and was one commit from writing a worse second copy of it.

**Both of the first two are now MEASURED even though neither is fixed**, by inverted checks that
assert the defect is present and announce their own retirement:
`tools/pdc_window_check.sh` and `tools/meter_publish_check.sh`. Each was verified by simulating
its fix and confirming the check reports "this has been fixed, delete this block" rather than
reading as a regression. So neither defect can drift while the decision is outstanding, and
whoever lands a fix gets told what to do with the check that was holding the line.
