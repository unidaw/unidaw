# Progress

A running account of what is being worked on and what came out of it. Kept in the repo rather than
in a chat log so it survives the session that produced it.

## Checked facts

<!-- EVERY NUMBER BELOW IS RECOMPUTED BY tools/progress_check.sh, WHICH RUNS IN ctest.
     They are measured against `as-of-commit`, not against your working tree, so you can commit
     freely and catch this file up in one go — but not indefinitely: the check also refuses when
     HEAD has drifted more than a dozen commits past it.
     Run `bash tools/progress_check.sh` and it prints the values to paste. -->

- as-of-commit: fb8ded2a
- main-cpp-lines: 2151
- main-function-lines: 1944
- ctest-entries: 229
- main-function-ceiling: 1944

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

## 2026-08-14 — the shared-memory boundary, and a stretch about evidence

Thirty-three commits, and almost all of them were about the same thing from different angles: the
boundary where this process starts trusting bytes another process wrote. What makes the stretch
worth reading is not the fixes — it is how many of my own claims did not survive being checked.

**AE-P1.3, the attach path.** `ring_view` took a ring offset out of a shared header, did
`base.add(offset)`, and DEREFERENCED it to read `capacity`, with `offset != 0` as the only test. The
out-of-bounds read happened before the first check, and the capacity and entry-size rules that
followed ran on whatever those bytes were. The C++ side was worse in kind: `mapSharedMemory` read
`mailboxOffset` and built a `completedBlockId` from it BEFORE the magic/version test whose entire
purpose is to detect a host built against a different layout. Both are ordered now — the size admits
a header, the header identifies itself, only then is any offset in it believed.

Then the part that mattered more: **validating a descriptor does not make the view safe to use.**
`write_entry` computed a masked slot index, explicitly discarded it, and indexed with the raw
reserved cursor; `drain_ui_out` masked the increment but not the first read. Nineteen region
accessors reached the mapping directly and now go through one bounds-checked helper, and a ratchet
refuses anything that reaches it another way.

**A pointer the producer loads atomically had five plain writes.** Most writers used
`atomic_store_explicit`; five assigned plainly, two of them under a mutex the producer never takes.
Mixed atomic and plain access to one object is a race whatever the values are. Separately, the
restart worker destroyed a `unique_ptr` — a use-after-free, not a stale read — outside the mutex the
producer reads it under.

**And the wire got an identity.** A refusal named a KIND of command, so a caller could not tell
whether the refusal was its own. Commands now carry a per-process nonce plus a counter in
`EventEntry::sampleTime` — bytes that were free because a command has no audio position — and the
engine echoes it. `kShmVersion` moved to 39, which is the first application of a rule written the
commit before: giving an existing field a new meaning is a wire change even when nothing observable
moves, because a version identifies which MEANINGS an image carries.

### What did not survive checking, which is the useful half

- **Two reviews refuted my REASONS and kept my FIXES.** The mapping guard was justified as closing a
  use-after-free; it closes nothing of the kind, because `TrackInfo` pins the mapping by
  `shared_ptr` — and a compare followed by a dereference could not close a UAF anyway. The fix is
  right for a different reason: it stops a dead host's stale samples being mixed.
- **Four populations were subsets.** Fifteen region accessors were nineteen; five plain writes were
  six; four atomic readers were eight; five arbitrated commands had been seven. Each time the
  predicate could not express what I was asking — a same-line pattern against a wrapped construct,
  `head` on an enumeration, a proximity window.
- **The same mistake reached a lock.** A grep line showed a `lock_guard` near a write and I inferred
  a nested scope; it was function-scoped and already held. The second acquisition self-deadlocked
  and a test timed out.
- **A control passed when it should have failed** — an exemption keyed by substring survived the
  rename it existed to notice, because the excused text was a prefix of the new text.
- **A check was red for a week behind my own sweep filters.** `version_arbiter` lost two members
  when undo became a whole-document operation and stopped being arbitrated against a per-track clip
  version. The removal was right; the floor was stale; the exclusion list that hid it was never
  re-examined.

The rule that comes out of all of it is narrow enough to use: **when the question is about
structure — scope, extent, enclosure, completeness — read the code, not a match.** A pattern's
output looks identical whether it found everything or could not express the question.

## 2026-08-07..14 — the architecture-excellence period, and what reviewing my own work cost

Eight days and ~350 commits under `ARCHITECTURE_EXCELLENCE_LEDGER.md`, which is where the ticket
state lives; this section records only what generalises. The period ran first as a fleet of four
workers and then, once that stopped producing verified work, as one agent doing the implementation
with subagents doing the reviews. **The reviews are the part that paid.** What follows is mostly a
list of things an independent reader caught in work I had already convinced myself of.

**A design refuted on two measured facts, both of which I had asserted.** The plan for carrying a
command id into the engine was to reuse the ring's reservation index, on the grounds that it wraps
at 2^32 — about fifty days of continuous traffic — and that `UiCommandPayload` had four spare
bytes. Neither is true. The index is MASKED at the ring capacity, so it wraps every 1023 commands,
about a second under load; and the payload has ZERO spare bytes, not four. The proposal died on
arithmetic I had presented as measurement, and the reviewer's whole contribution was reading
`apps/event_ring.cpp` instead of believing the document.

**The same reviewer found a live defect while refuting it.** A slow — not dead — producer that
publishes into a slot the consumer already retired gets its command dispatched one lap later, out
of order, seconds after the sender gave up. That is `docs/architecture/decisions/AE-RING-01-stale-replay.md`.
It is unrelated to the design being reviewed, and it was found because someone was reading the ring
carefully for another reason. Side findings are the argument for review by a party with no stake in
the conclusion.

**A justification I invented and then replicated seven times.** A commit message claimed a widened
field would break "four `reinterpret_cast` sites" in the host process. Those casts are of a byte
vector off a socket, and there is no such cast of the ring payload anywhere in the tree. The claim
was plausible, load-bearing for the decision, never checked, and copied verbatim into six more
places before anyone read it. Corrected as disavowals rather than deleted, because a wrong reason
that quietly disappears teaches nobody.

**A merge found the defect that seven reviews had not.** The contract-layout check compares
bindgen-generated twins field by field. Seven independent reviews passed it. Integrating it into
`main` is what surfaced the eighth defect: the set of structs it required was the UNION of every
candidate it had ever seen, so a struct deleted from the codebase — surviving only in prose
comments and one two-week-old build directory — became permanently mandatory. Fixed before the
merge landed, so `main` never carried it. Worth naming: **the reviews all asked "is this check
correct", and the defect only appears when you ask "what does this check do in a tree that has
moved on".**

**A red check is a place new breakage hides.** `doc_citation_check` was already failing on one
stale document. Four placeholder paths from unrelated work then broke it further and nothing
changed — the suite was red before and red after. Targeted `ctest -R` filters kept it out of sight.
An already-failing check has no signal left to give.

**"It fires" is not "it ratchets".** Several negative controls in this period emitted their named
failure reason while exercising a shape the pre-fix code already caught. The test that separates
them is to revert the fix and confirm the control goes BLIND. Four more controls were invalid in a
different way — their anchors had moved, so they asserted against text that was no longer there and
reported meaningless passes. Asserting that each anchor still exists is what caught those, and it
is now the habit: a control that cannot state what it would miss is not evidence.

**Where this leaves `main()`.** 1,992 lines to 1,944, by moving two captureless lambdas and their
rationale into `apps/engine_rt_helpers.h`. Captureless is the whole selection rule — they were free
functions already. The ceiling follows down to 1,944, which is what the ratchet is for, and
`progress_check` had been refusing commits for exactly the reason it exists: **it forbids raising
the ceiling and says to move logic out instead.**

Two items are blocked on an owner decision rather than on work, and both are recorded in
`docs/architecture/decisions/OPEN-DECISIONS-FOR-JAAKKO.md`: whether one credentialed capture may be
taken for the web-stack ticket, and whether giving an EXISTING field a new meaning requires a
version bump. The second gates two separate changes, and the tree currently argues both ways — the
command doctrine says such repurposing IS a wire change, and nine counter-precedents in the same
tree say it is not. That contradiction is the decision; picking a side inside a ticket would settle
it by accident.

## 2026-08-06 (afternoon) — a bug panel, and the regression it caused

An adversarial panel over the demo-critical surface: four finders with distinct lenses, deduped,
then each finding handed to an independent agent whose job was to REFUTE it, defaulting to refuted
when unsure. Six survived, five were fixed. It also killed three of its own finders' claims —
"the reverb processes silence", "a following observe shows pan 0.0", "the rehearsal triggers the
ring spin" — each of which would have had someone editing working code the day before a demo.

**The root cause it named is the useful part.** The agent tool layer is a THIRD COPY of the wire,
after daw-cli and the C++ payload defaults, and nothing in `tools/` drives an agent tool at all.
Every existing test either hand-builds the payload or drives daw-cli — the two copies that were
right. Three of the five findings were that: pan packed into a field the engine does not read for
pan, an insert index defaulting the opposite way to every other producer, and an argument the
executor reads that the schema never advertised.

### The fix that caused the regression

`add_device` defaulted its position to 0 — head-insert — while daw-cli and the payload append. I
changed it to append. That is right for an effect and **wrong for an event patcher**, which
generates notes for whatever follows it: `[sampler, patcher_event]` emits into nothing.

For about ninety minutes, asking the agent to put a patcher on a track that already had a sampler
produced a silent graph. **The demo rehearsal passed throughout**, because its patcher step counted
patcher devices in the chain — the device was present, the graph was valid, and only the order was
wrong.

One number was standing in for a rule with two cases. It is chosen by kind now, and both halves are
pinned, because fixing one direction is exactly how the other broke. The rehearsal's step counts
only patchers that could actually sound.

It was found by the web-UI agent asking a question — whether their console shared the head-insert
default — not by any test. Their console appends, with a test pinning it, which is wrong for the
same case in the same way. Two surfaces, two confident tests, opposite errors, one day. That is
task #113: the constraint belongs in the engine, because a rule three callers must remember is one
a fourth will forget.

### Two silences that were not what they looked like

The web-UI agent reported the patcher could not drive the built-in sampler: 0.1010 into a VST,
0.0000 into the sampler. I reproduced *a* silence, found that a slot pinned to one key cannot
answer a generated pitch, and told them that was their cause. It was not — their slot was already
full-range, and their actual fault was a zero-length placement scheduling nothing.

I had a measurement that explained a silence and offered it as explaining theirs. That is the same
move they had just retracted, one step removed. The phase it produced is kept, because the boundary
is real, with its comment rewritten to record a property rather than the story of an incident that
turned out differently.

Their narrower finding — `euclidean -> event_out` is silent without a middle node — was already
documented in `tools/patcher_plays_sampler_check.sh`'s own header, which says a euclidean emits
gates, a rhythm with no pitch, and the resolution path skips gates deliberately. The fixture's
author had fallen into it too. Whether `event_out` should promote a bare gate is an open question
for the owner; it is the obvious two-node gesture and it is the silent one.

## 2026-08-06 — rehearsing the demo, and the three things that found

The demo is Friday. `tools/demo_rehearsal.sh` drives the sidecar's command socket with one prompt
per demo claim and judges each by whether the ENGINE changed, so running it is the only way to
know the prompted path works today rather than last week. Running it three times found three
defects, and only one of them was in the product.

**The model could not write chords at all.** `add_notes` was its only way to put anything on a
track, so every chord it produced was frozen MIDI pitches and the harmony lane had nothing to act
on — which hollows out the demo's through-line, since "the harmony lane quantizes everything" is
only interesting if there is harmony to quantize. Chords had existed in the engine, the tracker
and daw-cli since long before the agent; only the surface anyone would ask through was missing.

`add_chords` writes DEGREES. They are ONE-BASED — `resolveDegree` coerces 0 to 1 and then indexes
with `degree - 1` — and the first draft of the manifest said 0 was the tonic. That would have
answered "I-V-vi-IV" with `[0,4,5,3]`, resolving to I-IV-V-iii: **the first chord right by
accident and every other one wrong**. The one that sounds correct is the one that hides the
mistake, so 0 is now refused with a message rather than left to the engine's coercion.

**The second write to a track was refused and reported as success.** Prompted for "kick on every
beat, snare on 2 and 4", the model made two `add_notes` calls, both returned `ok=true`, and the
saved project held pitch 36 and nothing else. The engine had said so and nobody was listening:
`clip.version_mismatch base=1..4 current=17 track=2`.

Two counters crossed. `add_notes` takes its base from `clip_version_for_track` — per track,
correctly, with a comment immediately above it explaining that reading the global is "exactly the
failure the per-track counters were introduced to end" — and then waited on
`wait_for_clip_version`, which polls the GLOBAL. The wait was satisfied by activity anywhere in
the song, so the call returned before its own writes were applied and the next one carried a base
that was already stale. `wait_for_track_clip_version` polls the right counter; a batch the engine
refuses ENTIRELY is now an error so the model retries, while a partial one only warns, because a
retry would duplicate whatever did land.

Mismatches across successive rehearsals went 80 → 4 → 0, and the snare lands.

**The third was the harness lying, twice.** The first rehearsal of the chord tool ran against a
sidecar built BEFORE the tool existed — the sidecar compiles in the manifest, so the model was
offered a stale tool list, hand-rolled a progression out of `add_notes`, and the step failed for a
reason that was not the code's. Twenty minutes went into reading that as a model failure.
`demo_rehearsal.sh` now refuses to run when the binary is older than the agent source. Separately,
`n_samplers` asked the kit read-back — a request/response round trip — once, immediately after
`add_device`, and reported 0 for a track whose chain held a sampler.

### The reproduction that could not fail

Worth recording on its own. The first test written for the dropped-snare bug put kick on every
beat and snare on beats 2 and 4 **in the same column**, so the snares landed on the kicks' ticks
and REPLACED them — one note per (tick, column) working exactly as designed. Sixteen notes either
way. The fixture could not tell "the second write was dropped" from "the second write worked
perfectly", and it took a diagnostic dump of the actual ticks to see it.

That is the same shape as every other fixture failure in this file: the test was measuring
something real and it was not the thing under test.

## 2026-08-06 (evening) — four diagnostics that pointed away from the answer

Not one of these was a wrong computation. All four were the machine describing itself
incorrectly, and each cost time proportional to how much the description was trusted.

**A number that meant nothing, printed where a meaning was expected.** `project.plugin_resolved`
logged `"slot": 0` for every plugin resolved off disk — `resolution.index` is 0 when the match is
None, and the direct-path case IS the None case. Entry 0 of u-he's Zebra2.vst3 is Zebra2, so a
project that had correctly loaded Zebralette printed a number saying it had loaded something
else. I believed the number over the parameter count for several minutes. There is no slot on
that path — the host picks the class by NAME — so it now says that instead of printing a zero.

**A known-failure note that outlived its defect.** `rust_tests_check`'s header declared
`multi_bundle_selects_named_subplugin` a genuine failure. It passes, three runs of three. A stale
note of that kind is worse than none: it tells the next person a working guard is broken, and it
supplies a ready reason not to run the one thing that would have corrected it.

**A timeout that was a refusal.** `note_overlap` flaked, and the kept evidence said why:
`write_note received`, then `write_note rejected:version`. The check waited for the NOTE COUNT to
publish, and the next `daw-cli do` — a fresh process — then read the CLIP VERSION, published on
its own tick. In the window between them the second writer reads a stale base and is correctly
refused, and the wait spends its full budget on a note that was never coming. **A refused edit
never arrives however long you wait**, so the failure message now says to look for
`rejected:version` before believing slowness.

**"Not found" about a device that was plainly there.** `OpenPluginEditor`'s resolver walks only
VstInstrument and VstEffect, so it returned nullopt for three different situations and the message
named the one that is usually wrong. Pointed at a sampler, it sent the reader looking for a
missing device. It now distinguishes all three, and it uses the existing `deviceKindToString`
rather than a second switch over DeviceKind — lifted out of an anonymous namespace for the
purpose, because a duplicated enum table goes stale one value at a time.

**And one where the negative control killed my explanation rather than the bug.** Two orphaned
host processes were found alive after 1h28m. The obvious cause — a SIGKILLed engine never running
`killHostProcess` — is DISPROVEN: measured with the engine SIGSTOPped so the harness must
escalate, and again with a direct SIGKILL and no cleanup path at all, every host was gone within
seconds *with the new reaper disabled*. A host exits when its socket peer dies. So the orphans are
real and unexplained, the reaper has no demonstrated effect, and its comment says so in those
words rather than claiming a fix. The alternative was a confident comment over code that has never
once fired.

## 2026-08-06 — the AI could add a sampler and never give it a sound

The agent shipped forty-one tools and not one of them could load a sample. `add_device` mints the
instrument; `SamplerLoad` was reachable from `daw-cli` and from the browser console and from no
tool at all. So "add a four on the floor kick pattern" wrote sixteen notes onto a **silent track**,
and `tools/demo_rehearsal.sh` scored that step a pass — because it counted notes.

That is the same failure this repo keeps meeting from a new direction: not a wrong answer, a
question nobody asked. Every check around the sampler asked whether the structure was right. None
asked whether the track could make a noise.

**`load_sample` reads the kit back rather than reporting the send.** A file name that resolves to
nothing still MINTS A SLOT — published with SOURCE MISSING and `length_frames` 0, a slot that
exists, draws, and is silent. A tool that answered "ok" on a successful write would say *loaded*
about a sampler that plays nothing, which is precisely the belief that then gets notes written on
top of it. It also says in words what the slot plays, because `key_low == key_high` meaning "every
other note is silent" is a step of reasoning, and it is the step that decides whether the part
sounds.

`device` is optional: 0 means the track's first sampler, which is how every handler in
`apps/engine_sampler_commands.cpp` already resolves it. On this wire an omitted field is usually
NOT a zero one, so it is worth saying that here it deliberately is.

**A sample name only resolved after a project had been LOADED.** `resolveSourcePath` treated an
empty `loadedProjectDir` as "resolve against the process working directory". That string is set
only by loading a project — saving does not set it — so a freshly started stack, which is how the
web UI comes up and what a person prompting "load a kick into the sampler" is sitting in front of,
resolved every bare name against the build directory. It now falls back to `defaultProjectDir()`,
which is what `HistoryJournal` already does with the same empty string.

**The check that should have caught it was green the whole time, and not for want of asserting.**
`tools/sampler_load_check.sh` asserts the load event, the slot ids, the fixed-pitch keys, the
refusals, and the save round trip. Early on it runs `cli do load blank` for unrelated reasons — and
loading a project is exactly what sets the variable under test. Every assertion after that line
inherited the precondition it was supposed to be checking.

Worth naming as its own shape: **not a missing assertion, a setup step quietly supplying the thing
being tested.** A missing assertion is visible by reading the check. This is only visible by asking
what the setup provides, which nobody does when the check is green. The new phase boots an engine
and never loads a project.

**Three wrong turns, each caught by running something rather than by thinking harder.** Matching
the read-back by file name — the engine seeds a slot's name with the file's *stem*, and a rename
command can change it afterwards, so the slot's identity is that it was not there before. Judging
a shell pipeline by pasting it into an interactive shell — that `grep` is a ugrep wrapper which
inverts `-qv` against the real one a script gets, and it reported a correct counter as broken.
And a first pass at the preset lint phase whose two negative controls both produced no output,
which read as "the controls did not fire" and was actually three separate bugs in the check.

**Duplicated work, third time this week.** The web-UI agent had built `load_sample` on its own
branch, as it had `add_chords` and the `add_device` ordering default before it. Neither of us can
see the other's branch, and the standing "announce before it lands" rule is scoped to the wire —
a tool is not an opcode, so none of the three tripped it. Proposed on the channel that the rule
extend to anything taken off a gap list, announced before the work rather than after.

## 2026-08-05 — an engine object, and what it was actually costing not to have one

`main()` went 2,072 → 1,985 and the wiring went from 536 hand-written positional arguments to 473.
Both numbers are in the checked-facts block; what follows is why they moved, because the shape of
the problem was not the one the line count suggests.

**Thirteen state groups already existed** — `TrackTable`, `TransportState`, `SongTiming`,
`ArrangeRail` and the rest, each in its own header documenting what it owns and which lock guards
it. That work was done and good. What was missing was anything that owned them *together*, and the
cost showed up as arithmetic: a deps struct needing six pieces of state named all six, at every
construction, in an order nothing but `tools/deps_order_check.sh` was checking. `apps/engine_state.h`
is that owner, and it is deliberately not another deps struct — **a deps struct answers "what does
this function need"; an engine object answers "what IS the engine"**, and only the second lets a
function take one reference instead of an argument list to keep in sync.

**Twenty-one structs converted, and the sweep stops there on purpose.** Sixteen more still name a
state group, and every one names exactly *one*. Converting those trades `TrackTable& trackTable`
for `EngineState& engineState` — one argument for one argument — and the function stops saying what
it needs. Widening sixteen narrow dependencies so the sweep looks complete is the opposite of the
point. `tools/deps_state_group_check.sh` enforces the threshold at two, keeps one legal, and derives
the group list *from `engine_state.h`* rather than keeping a second copy of it.

**The `std::function` adapters were lifetime bookkeeping, not structure.** About sixty
`const std::function<T> xFn = x;` lines sat in `main()` for one reason: a deps member declared
`const std::function<T>&` cannot bind a lambda, because the temporary dies at the end of the full
expression. Declaring 109 such members **by value** across 27 headers let lambdas bind straight
through and made every adapter redundant. That single change was 85 of the 87 lines. The two structs
built *inside* `produceBlock` — `RenderTrackDeps` and `NoteResolution` — are excluded and must stay
excluded, since a by-value `std::function` there would heap-allocate per block per track.

**What the remaining distance is made of, measured rather than guessed.** Of `main()`'s 1,985 lines,
737 are comments and 147 blank: 1,101 are code. The wiring passes 159 distinct names — 77 lambdas
and 81 loose locals. Those 81 are the anchor, because a deps struct cannot be constructed where its
inputs do not exist, and many of them are not engine *state* at all (`engineConfig`, `baseConfig`,
`audioBackend`, `consumer` are setup). Getting under 1,000 means moving the constructions and their
forwarders together into something that owns those too — one large move, not another sweep.

Two guards moved during this and both are worth knowing about. `deps_order_check`'s blindness floor
had to come **down** (480 → 450) because the argument count is falling on purpose; it is set just
under the live count rather than at a comfortable distance, so it keeps tripping and each drop gets
read. And `progress_check`'s ceiling refused stage A on its own — the aggregate cost six lines and
paid nothing back until the conversions landed, which is the correct verdict on a change that only
sets up future work.

## 2026-08-05 — three protocols that were published, complete, and read by nobody

A run of defects with one shape: the engine held up its end, the value travelled, and the reader
never looked. None of them could be found by reading the producing side, because that side is
correct in each case.

**`daw-cli get clip` returned one page of a paginated protocol.** The clip window carries at most
`kUiMaxClipNotes` notes; past that the engine stops early, reports where it stopped in
`nextEventIndex` and withholds `kUiClipWindowFlagComplete`. It has honoured the cursor since the
protocol was written. Every client sent cursor 0 and read one answer, so a dense clip came back
truncated with exit code 0 and nothing on stderr — **indistinguishable at the call site from a clip
that really is that short**, which is what makes it worth a loop rather than a warning. The fixture
in `tools/clip_window_paging_check.sh` needs THREE pages, because a client that learned to fetch a
second page and stop would pass a two-page one.

**The sidecar's header offsets were 48 bytes stale, and the page was right the whole time.** The
`lpb` block went 8 → 16 → 64 bytes; the trailing offset comments in `encode()`, its three
checkpoint assertions, and the offset test's literals all still named the 16-wide numbers. So the
encoder asserted that the song meter starts at 132 while writing it at 180: **any debug build of
the sidecar panicked on its first frame**, and release was fine because `debug_assert` compiles
out. Note the asymmetry that made it undetectable — `ui-web/src/wire.js` has to spell offsets out
and the encoder just writes in order, so only the Rust side could be wrong and only the Rust side
had no way to find out. The offsets are now `debug_assert_eq!` per field (free in release, and it
catches two adjacent same-width fields swapped, which no total length can), and the test reads its
numbers out of `wire.js` instead of keeping a second copy.

**Nobody had built the Rust test binaries.** That is why the above survived: `WaveformSlotView`
gained a field, one test's initialiser was not updated, and `cargo test -p daw-sidecar` stopped
COMPILING — 75 tests absent from every run while `cargo build` stayed green. `check_registry_check`
asks whether every `*_check.sh` is registered; it cannot ask whether a whole language's suite
exists. `tools/rust_tests_check.sh` now runs `cargo test --workspace --no-run` in ctest, which is
the assertion that would have caught it on the commit that introduced it.

**A project naming a plugin by path loaded a different plugin.** A saved device carries a durable
`vst_ref` and a `host_slot_index` — an index into the scan of the machine it was saved on. When the
ref did not resolve but the path was on disk, the loader kept that index, and the host, which only
consults `vst_ref.path` when the slot is the Direct sentinel, looked up someone else's number.
Measured rather than argued: a device naming `Zebralette` logged `plugin_resolved
match:"direct_path"` and then instantiated `Identity`. "Load it by path" is not something the
loader can express by doing nothing.

That rule had **three copies**. The master track's had only the cache-hit half — neither the
on-disk case nor the unresolved one — sitting directly beneath a comment describing exactly the
failure it permitted, on the one track everything else is summed into. All three now go through
`daw::resolveDeviceSlot`, with the four outcomes unit-tested including *"the slot was already
Direct, leave it"* — the case whose absence once made seven audio checks render silence.

The through-line: **a published field with no reader is not a feature, and nothing in this repo
was measuring readership.** Three of the four were found by following a value forwards from the
code that writes it, and the fourth by running a test suite that had not been run.

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

### Waiting on Jaakko specifically (2026-08-06)

Collected here because the demo follow-up will want them in one place, and because each has been
sitting in a task list where an owner does not read it.

- **FILL conditional trig — what fill state does a BOUNCE render under?** A FILL trig fires on a
  fill; an offline render has no performer pressing anything. Rendering it as never-fill, always-
  fill, or a documented default are all defensible and the choice is musical, not technical.
- **A sampler track cannot be an audio routing destination** — the sampler overwrites its input
  rather than mixing into it. Whether a sampler should mix or replace is a design decision.
- **Should an event-out promote a bare GATE to a default degree?** A euclidean emits gates, which
  carry no pitch, and the note path skips them deliberately — so `euclidean → out` is silent BY
  DESIGN and the fix for a user is to put `random` or `slice` between. Promoting silently would
  make a gate into a note, which it is not. Real design call.
- **`SamplerSetFilter` writes the filter TYPE unconditionally, with no set-flag.** Cutoff and
  resonance each have one and can be left alone; type cannot. So a caller adjusting cutoff turns
  the filter OFF on the way past unless it re-sends the type, and returns success. Found by the
  web-UI agent, who worked around it by requiring the type in their tool. Adding
  `SAMPLER_FILTER_SET_TYPE` is a contract change and would be announced before it lands.

### Open and simply not understood

- **Two `juce_host_process` orphans were found alive after 1h28m** and the obvious explanation is
  disproven: a host exits on its own when its socket peer dies, measured with the engine
  SIGSTOPped into the SIGKILL path and with a direct SIGKILL, both with the new reaper disabled.
  The reaper in `tools/lib/engine_wait.sh` has never been observed to fire and says so. The first
  time it does fire is the first real evidence about the cause.

- ~~**Published meter points carry the requested tick, not the effective one.**~~ **FIXED
  2026-08-05** — kept for the reasoning, struck so a scanner is not misled by the bullet. The
  correction used to live only in the paragraph at the bottom of this section, which is precisely
  how a reader skimming bullets ends up believing a closed item is open.
  Original text follows.
- **Published meter points carry the requested tick, not the effective one.** `setMap` snaps a
  signature change forward to the next bar line; the wire publishes the tick that was *asked for*.
  Measured: 3/4 requested at 5 quarters, published as 5, actually begins at 8. Fixing it properly
  means widening `UiTimeSigPoint` and bumping `kShmVersion`, which the equality gate turns into a
  forced rebuild on both sides — so it needs coordination, not just code.
- ~~**PDC clamps every event in the first `latencySamples_` to sample 0**~~ **FIXED 2026-08-05.**
  Struck for the same reason as the bullet above. Original text: PDC clamps every event in the
  first `latencySamples_` to sample 0, collapsing their relative timing; three candidate fixes,
  and choosing among them is an owner's call. (The offset those options traded off turned out to
  be inert — the "three options" had no options in it.)
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

~~**Both of the first two are now MEASURED even though neither is fixed**~~ — **BOTH ARE FIXED
(2026-08-05), and this paragraph was left standing after they were.** `tools/pdc_window_check.sh`
and `tools/meter_publish_check.sh` were inverted checks asserting the defect was present; each
announced its own retirement, each went red when the defect went away, and each is now inverted
back to assert the correct behaviour. A test judge found this paragraph still claiming otherwise
in a file edited AFTER the fixes landed — `doc_citation_check` passes because it verifies that
named files exist, never that a claim about them is true.

Both were pinned rather than fixed on a cost estimate that was never re-derived: one was "an
owner's call between three options", and the offset those options traded off turned out to be
inert; the other was "a contract change that bumps kShmVersion", and only the VALUE was wrong.
See the memory note on pinned defects overestimating their fix.
