# AE-P1.2 — SHM contract phase packet

**Successor to `e0f83e43d55a3682be0753e00d546b3be8b29c25`**, which is superseded and not amended.
AE-P1.1 is FROZEN at `ba88bcb4657b62bdfc752d338d877e139e212ca6` and is untouched by this packet.

**Product under review is FROZEN** at `75c6f0646417828641e43287c260bea3d38b5a6f` (tree
`699abfe8f72e597cfd1b6fb3da93ee7f35aa7fef`), read from a pinned read-only checkout. Every citation is
a line at that SHA. This packet SPECIFIES; it does not implement, and no product edit was made.

## Why this supersedes e0f83e4

The predecessor promised that every gate carried seven elements and delivered them for one gate of
eight. That is a claim wider than its population, in the document whose own rule forbids it. The
cause is worth recording because it was not haste: under cost pressure the CONTENT was compressed and
the PROMISE about the content was left standing. Abridging is legitimate; abridging under a universal
claim is not, and the two must move in the same edit. A dangling reference to a "gate table" that was
never written is the same error's fingerprint.

The blockers from the exact review are reconciled here, and the count is deliberately not restated: it moved between rounds and a fixed number here would be a twin of the item list, which is the authority:

1. **GATE PLANNING VERDICTS — plannable 0, unknown 8, blocked 0.** **OWNER CHOICES (G0-A×2, G0-B×1,
   G1-A×2, G1-B×2, G2-A×1, G2-B×1, G3×3, G4×1)** and **NON-GATE PREREQUISITES (G1-B)**, the first derived from every
   `SHALL decide` / `SHALL rule` / `SHALL choose` demand in a gate's review register and the second
   from the gates' `Dependencies` clauses, both compared against this restatement. **The choice
   population was MARKER-AUTHORED for one SHA and I had five wrong**: codex-worker-1 found five live
   demands unmarked and G3's two choices sharing one marker, so the emitted reason lost the second
   entirely. The register's own VERB is the distinction I had said could not be read — this packet
   writes `SHALL confirm` for a fact and `SHALL decide` for a choice — and a rule over it beats a
   marker, which is a population I can forget. **I then claimed it was COMPLETE BY CONSTRUCTION and
   it is not**: `SHALL confirm` carries G0-A's refuse-versus-degrade choice and `SHALL establish`
   carries one in G1-A, so the verb set is a FLOOR over a named list and the population is at least
   what it finds. Claiming completeness for a rule written in the same minute is the overclaim the
   marker set made, one mechanism along. Three further repairs came with it, each a defect this
   packet has already recorded elsewhere: `SHALL also rule` was missed by requiring the verb
   ADJACENT to SHALL, the extractor scanned the whole gate rather than the REGISTER, and sentences
   ended at any period so a reason truncated inside `control.rs`. Every gate names a population
   it can range over, which is what this line used to report as "EVERY GATE IS PLANNABLE AT THIS
   SHA"; that claim read TWO, then ONE, then every, across three SHAs, and it is **withdrawn as
   OVERSTATED rather than as wrong.** No gate's own population is withdrawn — the fact it was
   built on still holds — but planning also depends on a gate's NON-GATE prerequisites, and open
   item 38 (G1-B) records that whether a live one defeats planning is UNRULED. G1-B carries one,
   and the five gates whose dependency closure reaches G1-B inherit the same unknown. Publishing a
   boolean `true` there stated an answer this packet does not have, so the verdict is now
   three-valued and `plannable_with_dependencies` is GONE rather than overloaded — a tri-state
   string in a boolean's key reads as TRUE in every consumer written against `/5`. **Declining to
   overstate is not deciding**: nothing here rules item 38, and the counts move the moment someone
   does. codex-worker-1 named the false precision and the repair. It is still a small claim: G4 left when its
   population was completed across BOTH halves of its invariant; G2-A left when R12 authored its
   scope as the adoptable-refusal EMIT sites, every earlier attempt having selected on the
   arbitration while the gate is about the answer. Both remain BLOCKING for acceptance, for defects
   in their TESTS and in the PRODUCT respectively — **a plannable gate is not a decidable one**, and
   the manifest keeps the two apart deliberately. The zero case is a different sentence rather than
   a missing one, so the check cannot pass by the claim disappearing. G1-B's readers were withdrawn and are AUTHORED again under R1
   with rules, members and drift detectors; G2-A's scope is AUTHORED under R12 and G4's population is complete across both planes — this sentence said neither was, and outlived both. **TWO GATES ARE ACCEPTANCE-DECIDABLE — G0-A and G1-A —
   and eleven items block (18, 19, 24, 26, 27, 28, 29, 33, 35, 36 and 37)** and ALL ELEVEN need product
   work. **Item 37 needs a PACKET EDIT BESIDES**, and the opening used to imply otherwise: G3's
   acceptance surface cannot see the defect that item names, so writing its oracle into the
   deterministic test, PASS conditions and static checks is packet work. None of the eleven is
   currently waiting on another item — **classification and state are orthogonal
   and the markers no longer pretend otherwise**: ⟦PRODUCT⟧ says what KIND of work closes an item,
   ⟦BLOCKED-ON: n⟧ says what it WAITS ON, and an item may carry both. Item 27 carried an edge to
   item 29 for three SHAs and it was WRONG: codex-worker-2 ruled that item 29 is only the narrow
   `SetRowOps` zero-base defect, while the class-wide failure — no sender-minted instance identity
   at ANY of the three emit sites — is already item 27's own content and R12's. **An item does not
   delegate to a narrower item that happens to be nearby.** The counts are derived from the markers
   (`BLOCKER-KIND`). The previous figure was three, and a lexical scan for the words "product work"
   finds only two, because items 19, 24 and 26 need product work and say so in other words entirely.
   **The derivation is only as good as the marking, and the marking is mine** — this check can catch
   a marker missing from a blocker, and cannot catch a blocker marked wrongly; item 27 was marked
   ⟦PRODUCT⟧ for one SHA when its scope is authored and its blocker is item 29's defect. **A classification cannot be read out of prose that was not written
   to carry it.** FIVE blockers are packet edits as well as product fixes — item 28, whose
   extraction predicate, command and member list have never been authored while a counter-only
   decision survives at `ui/daw-cli/src/main.rs:1183-1184`; item 26, whose gate needs one
   fixture authored — an adjacent multi-plugin segment — and the oracle for it; this used to say
   G4's PASS conditions, deterministic test and static checks were written against one plane, which
   item 26 has since withdrawn as false because S2, S5, PASS 2 and PASS 4 already cover input; item 37, whose
   acceptance oracle is unwritten; item 36, whose Layer 0, PASS 1 and register text all need
   rewriting alongside the telemetry it waits on; and item 18, where G2-B's PASS 4 is withdrawn with
   no replacement, so no PASS condition accepts the two-level implementation that exists and writing
   one is packet work. Each carries ⟦PACKET⟧ beside ⟦PRODUCT⟧ so the
   manifest can say so; until `kind` became an array that fact was unrepresentable and the record
   said PRODUCT. An earlier version of this sentence named only item 37. **⟦PACKET⟧ MARKS (18, 26, 28, 36, 37, 38, 39)**, derived from the item heads and compared against this restatement, because representing a
   kind is not binding it: deleting the marker, ADDING one to a nonblocking item, DUPLICATING it and
   misspelling it all passed until these checks existed — and a two-site MOVE is not expressible in
   the single-anchor control format, so no control claims to perform one, and a marker parked later in a head
   validated as present while emitting as absent because the validator and the emitter read the
   head differently. There is now ONE derivation of a kind, parsing the LEADING marker run, and both
   sides read it. A control performs each mutation. That list is derived from the items themselves and every
   restatement of it anywhere in this document is compared against the derivation
   (`BLOCKER-SET-RESTATED`) — this paragraph carried (18, 19, 23, 24, 29) for several SHAs, three
   members wrong on the packet's first screen, because the check knew about the open-items header
   and not about the sentence that repeats it. **That set is DERIVED and checked (`GATE-ACCEPT-PROSE`)**, not asserted. It read "No gate is
   acceptance-decidable" until the check that derives it was written, and the check failed on its
   first run: three gates carried no blocking item. It is TWO now — G1-B left the set when item 33
   was filed, because restoring its withdrawn PASS conditions revealed that the tests they name do
   not exist. **The withdrawal had been carrying the gate's acceptance**: a withdrawn condition is
   not a failing one, so nothing contradicted the flag while the words stood. The sentence had been
   true when the packet was younger and was never re-decided, which is the fourth stale universal
   this opening has carried.
   The manifest publishes both senses as `decidable_for_planning` and `decidable_for_acceptance`,
   and G3 shows why one boolean will not do: R3 makes it plannable and it is not acceptance-decidable
   until the N ticket lands. **Acceptance also has an `acceptable_with_dependencies` form; planning's
   equivalent is the three-valued `planning_verdict`, which closes over dependencies in its own
   right** — this sentence said "each also has a `_with_dependencies` form" for one SHA after the
   planning one was removed. Both exist because the local flags
   were the whole answer and a gate's dependencies were emitted and never USED: G4 published
   `decidable_for_acceptance: false, blocking_items: [26]`, so a consumer closing item 26 would have
   read G4 as acceptable while G0-B, G2-A, G2-B and G3 — every one of them a declared dependency of
   G4 — stayed unacceptable. `acceptance_blocked_by` now names that closure. An edge present in the
   data and absent from the conclusion is the same defect as a named hole emitted as an empty field.
   **This paragraph is where every stale claim in the packet has accumulated**, because it is
   written once and thereafter read as framing rather than as claims; it now states no number that
   is not derived elsewhere and checked here.  Every gate carries the record's SHAPE — population slot,
   failure model, deterministic test, PASS conditions each naming their refutation, static checks,
   review register — and for those three the population slot read "withdrawn" when this was
   written, which satisfied the shape and decided nothing. **G4's slot no longer does** — its
   population is complete across both planes and its blocker is the missing adjacent multi-plugin
   fixture, per item 26. **No universal claim about populations is made anywhere in this
   packet**: a sentence of the form "each population carries its extraction command" is false at
   this SHA by construction, and this paragraph previously ended with one while opening with the
   disqualification, because I patched its head across three rounds and left its tail alone.
2. **The list is 39 atomic items, of which 30 are open — not 15 categories.** The word
   "atomic" counts the whole list including CLOSED entries, and this sentence read "the open
   list is 39" while the header said 30 open; backend caught the two readings. The four that compression swallowed are
   restored: G0-B's unowned mutation floor, G2-A's BATCH blindness, G2-B's probe-order false-green,
   and G3's debug-env requirement plus its self-contradicting static check.
3. **G0-A's mailbox census was wrong twice and is corrected with its method.** See G0-A.
4. **G2-B reaches bypass and `sendSetBypass` failure**; its mirror-replay bullet is WITHDRAWN as
   circular rather than reworded, and the mirror half is open item 18 (G2-B).
5. **G4 now states the full ordering** `write_output → release-ack → acquire-wait → read_output`, and
   its negative control is respecified so it can run.
6. **G2-A's room arithmetic is corrected and the correlator population completed.**

## Provenance of the findings

Eight findings arrived from an independent semantic audit of the frozen product and were verified
against the same SHA by a separate reader, each confirmation attacked by a refuter. That pass changed
the input, and the packet records the change rather than the original:

- **F8 REFUTED as a live defect.** Two patcher exports do lack an `abi_version` guard, but no sequence
  at this SHA delivers a wrong version to them. G0-B is re-scoped to the `PatcherContext` layout pin
  that is actually missing; the guards are hardening.
- **F1 CRITICAL → HIGH**, bounded to three chain shapes, because the default path is already gated by
  the mechanism its gate generalises.
- **F4 was argued down to LOW and that was WRONG.** The reviewer's rebuttal held on the code: the
  argument for LOW was a threat-model answer to a robustness claim. HIGH at the C++ host boundary.
- **F3's reclamation** belongs to the C++ consumer, not the Rust side as first reported.
- **F7's invariant reworded**: a caller does not learn a *different command* applied; it **adopts
  another command's terminal outcome** for its own.

Severities of record: F1 HIGH · F2 HIGH · F3 MEDIUM/MEDIUM-HIGH · F4 HIGH (C++ host boundary), LOW
(Rust UI attach, allocated to its own gate, not this packet) · F5 MEDIUM-HIGH · F6 HIGH · F7
MEDIUM-HIGH · F8 re-scoped.

## Gate sequence

    G0-A → G1-A → G1-B → G2-A → G2-B → G3 → G4
    G0-B → G4

The diagram has now contradicted the text TWICE. It showed `G1-A ∥ G1-B` while G1-B's Dependencies
line names G1-A; that was fixed, and the fix introduced `G0-A ∥ G0-B → G1-A`, which asserts an edge
G0-B → G1-A that G1-A's own line does not have — G0-B is a dependency of G4 alone. **A picture that
disagrees with the text it illustrates is worse than no picture, because it is the part a reader
trusts without checking**, and a picture is also the part nobody re-derives when the text changes.
So it is no longer only a picture: every `X → Y` in the block above is now checked against the
dependency lines (`DIAGRAM-EDGE`), which is the only reason to believe this third version.

**No downstream end-to-end success waives a failed primitive gate.** A green G4 over a red G1-A means
the G4 fixture did not exercise the primitive. Every test exposes deterministic barriers; a timing or
stress result is not evidence and is not accepted in place of a transition test.

## The record each gate carries

POPULATION with the command that produced it and the FLOOR below which that extraction is blind ·
FAILURE MODEL · DETERMINISTIC TEST · PASS conditions, each naming what REFUTES it · STATIC CHECKS for
what no runtime observation can decide · REVIEW REGISTER for what no mechanical check can carry ·
DEPENDENCIES. Where a gate cannot mechanically decide something, that is stated as a register entry
rather than dressed as a passing bullet.

---

# G0-A — Validated mapping geometry (C++ engine side of a host-created segment)

**Severity** HIGH. **Dependencies** none; first gate.

**Scope.** GOVERNS the engine's consumption of a mapping created by a hosted-plugin child:
`apps/host_controller.cpp`'s attach path and every engine-side non-test access reached through
`HostController::shmHeader()/mailbox()/shmSize()/sharedMemory()` or the `TrackInfo` copies at
`apps/engine_consumer.cpp:648-672`. EXCLUDED, each named so the invariant does not silently widen:
the engine's own UI segment (`apps/engine_ui_shm.cpp`); `ui/daw-bridge/src/control.rs` attaching that
segment — a real LOW defect at `control.rs:1952-1971`, **allocated to its own gate outside this
packet**, which the predecessor excluded without allocating; the child's own accesses; and the
unbounded test harnesses at `apps/phase2_tests_main.cpp:184/:197` and
`apps/phase3_tests_main.cpp:164/:177`.

**Invariant.** The engine must not load or store through a pointer into a host-created mapping unless
the whole byte span lies inside the object, decided against a length the engine produces WITHOUT
trusting the peer.
**(a) Length** — `mmap`'s length and every later bounds anchor is `S_expect = daw::sharedMemorySize(H, …)`
with `H` from the engine's own `HostConfig`; `response.shmSizeBytes` may appear only as the left side
of an equality test. **`fstat` st_size is REQUIRED as a floor** (refuse if `st_size < S_expect`), not
optional: without it an object truncated between `sizeof(ShmHeader)` and `S_expect` still faults on
its last page.
**(b) Floor** — the mapping is known to be ≥ `S_expect` bytes of real object before any `ShmHeader`
member is loaded.
**(c) Regions decided on the INPUTS** — one validated-layout computation whose inputs are checked. A
range test on an already-computed offset is insufficient: a `uint64` `audioOutOffset` near 2^64 wraps
`auxOutputPlaneOffset` SMALL and passes the end-of-region test at `apps/engine_consumer.cpp:775-776`
and the span rule at `apps/engine_rt_helpers.cpp:258`.
**(d) Order** — no header member but magic/version is loaded before the magic/version comparison, and
no derived pointer is published before every gate passes. At pin both are violated: field load at
`host_controller.cpp:291` against a gate at `:298`; publications at `:294-297`. Order is **not**
observable from a return value, so (d) is decided statically.

**Population.**
- *Regions the engine addresses* — 8, exact. [HAND-CLASSIFIED — open item 25 (all)] The rule as
  previously written — "every `ShmHeader` offset field read off a controller-derived header plus the
  two derived functions" — yields SEVEN and the population is eight: it omitted the mapping/header
  base itself, which is a region the engine addresses and is not an offset field. The members are
  the mapping/header base, `audioIn`, `audioOut`, `ringStd`, `ringCtrl`, `mailbox`, `auxOutput` and
  `hostKeyRing`. A hand-classified population whose stated rule does not reach its own member count
  is the worst case of the category, because it reads as derived and is not — the members are listed
  here so the gap is visible rather than arithmetic.
- *Raw region derivations outside any validator* — RAW 13 (`grep -rn -e '>audioInOffset' -e '>audioOutOffset' -e '>ringStdOffset' -e '>ringCtrlOffset' -e '>mailboxOffset' -e auxOutputPlaneOffset -e hostKeyRingOffset apps/ | grep -v _tests_main | grep -v juce_host_process_main | grep -v engine_ui_shm | grep -v audio_shm | grep -v shared_memory | grep -v uiShm`) returns 13 with the exclusions carried INSIDE the
  pipeline (the predecessor said 12, and stated the exclusions in prose beside a command that did
  not apply them) → minus 1, `apps/ipc_protocol.h:46`, which is a COMMENT naming `hostKeyRingOffset`
  and not a derivation → **12 executable derivations**. ⟂ A grep over source text cannot tell a
  mention in a comment from the code it describes, and comments are where the warnings about the
  code live, so a text census will always over-count in exactly the place the code is most carefully
  documented.
- *Bounds checks anchored on the child's number* — 7, exact. [HAND-CLASSIFIED — open item 25 (all)]
- *Ring constructions over a host-created mapping* — 3, exact. [HAND-CLASSIFIED — open item 25 (all)]
- **Mailbox `completedBlockId` LOADS — 7 live, 8 syntactic.** Command:
  `grep -rn 'completedBlockId' apps | grep -v -e _tests_main.cpp -e juce_host_process_main.cpp | grep -E '\->load|\.load'`
  returns 8: `engine_produce_block.cpp:910` (sidechain), `engine_audio_callback.h:284`, `:328`,
  `:951` (RT thread), `engine_consumer.cpp:762` (direct, via `shmView->completedBlockId`),
  `engine_master_render.cpp:83`, `engine_producer_thread.cpp:239`, and `watchdog.h:47`.
  **`watchdog.h:47` is DEAD**: `Watchdog::check` has no production caller — which is G3's own finding,
  so counting it as a live reader would make this packet contradict itself across two gates.

**Floor.** Only the DERIVATION census is a name-grep, and it is exact because the names are the
mechanism. The region, bounds and ring censuses are NOT name-greps and this paragraph said they were
— they carry hand-classified markers on their own headings (the marker
text is described here and not reproduced: a marker quoted in prose is indistinguishable from a
marker, which is how this paragraph came to hold a seventh one attached to nothing), so the floor
paragraph and the population contradicted each other within one gate. A semantic grouping has no
floor of the kind this paragraph describes: its blind spot is not "a name the grep missed" but "a
member a reader did not think of", which no command bounds. The mailbox census is a **pointer-flow** census and is a FLOOR of 7 live: a
new `TrackInfo` copy is invisible to it. **This census was stated as SIX and then as TEN, and both
were wrong** — six omitted the three RT loads and the direct load; ten added `watchdog.h:47` (dead)
and `engine_producer_thread.cpp:209` (not a load). It is reported with its method so the next reader
can re-derive rather than trust it.

**Failure model.** (1) The engine has no means to check the advertised size — one value crosses the
socket and nothing reconciles it. (2) Over-advertised size: `mmap` past the object succeeds; the first
load in a page beyond it is **SIGBUS in the engine**. (3) Under-advertised/truncated object, which
hits earlier and which the predecessor omitted: no floor exists on the C++ side. (4) The
over-advertised number is the anchor of all seven existing bounds checks. (5) `channelStrideBytes` is
absent from the config gate at `:304-315`. (6) Mailbox at an arbitrary offset, read on four threads
including RT.

**Deterministic test.** Three parts. **A** drives `connect()`/`launch()` against a stub host installed
via the existing `DAW_HOST_BINARY` hook (`host_controller.cpp:124-128`), one row per geometry
mutation. **Rows named by the PASS conditions, defined here:** A1 = a canonical, correctly sized,
correctly offset object (the only row that must attach). A2 = advertised size larger than the object.
A4 = an 8-byte object advertised as 8. A5 = advertised size smaller than `S_expect`. A7 = mailbox fits
exactly, derived key ring does not. A12 = `audioOutOffset = 2^64 − k`, wrapping `auxOutputPlaneOffset`
inside the header. A13 = a valid object under a name the engine did not choose. A15 = a valid object
whose header is mutated after attach. The stub links `apps/ipc_io.cpp` rather than hand-rolling framing, because `recvHeader`
rejects on `kControlVersion` and a hand-rolled constant turns every row into a handshake refusal that
looks like a pass. **B** drives the region half, which `connect()` never reaches — the mailbox poll,
both event rings and the key ring are constructed after attach. **C** is static.

**PASS conditions.**
1. Length provenance: `mmap`'s length and `SharedMemoryView::size` are `S_expect`. *REFUTED BY* the
   ratchet finding `response.shmSizeBytes` in any other position.
2. Row A1 and only A1 attaches. *REFUTED BY* A1 refusing — the anti-trivial half, without which the
   table is satisfied by an engine that refuses every host.
3. Every refusing row refuses by the mechanism it names, read from a typed `lastAttachError()` whose
   enum **includes `MmapFailed`**. *REFUTED BY* A5 refusing as `MmapFailed` rather than `SizeMismatch`.
4. The floor is at `S_expect`: an object of `S_expect - 4096` refuses with `ObjectTooSmall` and the
   forked child exits normally. *REFUTED BY* the child dying on a signal — the pinned behaviour.
5. Raw region derivations outside `validateHostLayout` number 0. The validator lives in
   `apps/shared_memory.cpp` so the ratchet's path exclusions can express "outside the validator".
   *REFUTED BY* the command returning any line outside it.
6. The validator refuses on the INPUT: the overflow row is an offset-field wrap
   (`audioOutOffset = 2^64 − k`). The predecessor's row posited `channelStrideBytes ≈ 2^58`, which is
   **unstorable** — `uint32_t` at `apps/shared_memory.h:185` — and that impossible premise had reached
   five places including a PASS bullet. *REFUTED BY* the wrap header returning a valid layout, or being
   refused as `RegionOutOfRange` (the result was checked, not the input).
7. The host key ring is in the population. *REFUTED BY* the mailbox-fits/key-ring-does-not row
   attaching.
8. Order decided statically, ratchets bite. *REFUTED BY* any ratchet passing against pinned
   `host_controller.cpp` — a ratchet that passes on the known-violating artifact matches a name, not
   an order.
9. Negative control flips the verdict and proves it ran, printing a build stamp. *REFUTED BY* the
   suite passing with size validation removed, or a passing control with no stamp — indistinguishable
   from a sabotage that never reached the binary.

**Static checks.** S1 pre-gate field load · S2 pre-gate publication · S3 single-validator routing,
whose exclusion set is **five path exclusions plus one pattern** (`uiShm`); the pattern is the
accident-prone kind and is named as such rather than disclaimed · S4 each of S1–S3 run against a copy
with the violation restored · S5 `response.shmName` reaches `shm_open` only after a length-bounded
read.

**Review register.** The reviewer SHALL decide post-attach shrink, which no validation closes
because the child keeps its fd. The reviewer SHALL decide the expected verdict for a valid object
under a name the engine did not choose. **Both are live CHOICES OF BEHAVIOUR, not confirmations of
fact**, and a planner cannot write the validator's ticket without them. The reviewer SHALL confirm: refuse-versus-degrade for a bad ring offset; that `mmap` past the object
succeeds on this platform, nothing here having been executed; that the fixture asserts `offsetof`
rather than trusting a hand-computed byte 96; that the enumerations were RE-DERIVED at the delivering
SHA; that all **seven live** mailbox readers take their pointer from the validated layout; whether
`shmHeader()` can be null at `apps/daw_engine_main.cpp:955`; that the Rust UI attach gap has been
allocated to a named gate; and that the unbounded test harnesses are brought onto the validator or
removed, so they do not stand as the pattern to copy.

---

# G0-B — PatcherContext layout pin

**Severity** re-scoped from the finding as received. **Dependencies** none.

**Why re-scoped.** The finding — `patcher_process_lfo` (`patcher_rust/src/lib.rs:745-753`) and
`patcher_process_audio_passthrough` (`:808-817`) lack the `abi_version` guard their three siblings
carry — is NOT a live defect at this SHA: all seven `PatcherContext` sites are `daw::PatcherContext ctx{}`, applying the NSDMI at `apps/patcher_abi.h:114` before their explicit assignment; a repo-wide
grep shows `abi_version` is never sourced from SHM, a project file or a UI command; `patcher_rust` is
a staticlib with no `dlopen`. **The guard is also not the mechanism it appears to be:** `abi_version`
is field 0, so it catches only "layout changed AND version bumped AND artifact stale". In the sibling
case — layout changed WITHOUT a bump — the GUARDED exports corrupt identically:
`patcher_process_euclidean` passes `:287` then takes `event_buffer`/`event_capacity`/`event_count`
from shifted bytes at `:290-293` and writes 64-byte `EventEntry` records through them. Nothing pins
the layout: `apps/patcher_abi.h:172-188` static_asserts four types and says nothing about
`PatcherContext`; `lib.rs:35-38` covers only `HarmonyEvent`. The two guards remain as hardening.

**Invariant.** For the eight types crossing the patcher node ABI, the C++ definition as a compiler
lays it out and the hand-written Rust mirror must agree on `sizeof`, `alignof`, and per member: byte
offset, byte size, normalised name, and normalised TYPE FORM — integer width AND signedness, float, or
a pointer spine with per-level mut/const and a normalised terminal type. Type form is in the tuple
because offset+size+name are together blind to `float sample_rate` becoming `uint32_t`.

**Population.** *ABI types* — 8. Command: `grep -n '#\[repr(C' patcher_rust/src/lib.rs` returns 8 (lines 23, 40,
55, 68, 82, 89, 97, 107), **NOT cross-checked against bindgen**: the packet claimed a
cross-check against an `allowlist_file` closure over `apps/patcher_abi.h`, and no `build.rs` exists
under `patcher_rust/` and no `allowlist_file` appears anywhere in the tree. The claim described a
generation mechanism that is not there, so the two sides of the ABI are joined by NAME and OFFSET
only, which is exactly why the `reserved`/`_pad0` mismatch at item 24 matters. *Members* — 66 C++ / 65 Rust, each produced by a command rather than by reading, which is what item 5 (G0-B) required while it was open. C++: `awk '/^struct (alignas\([0-9]+\) )?(HarmonyEvent|MusicalLogicPayload|PatcherEuclideanConfig|PatcherSliceSelectConfig|PatcherRandomDegreeConfig|PatcherLfoConfig|PatcherContext|EventEntry) \{/{inb=1;next} inb&&/^};/{inb=0;next} inb{l=$0;sub(/\/\/.*/,"",l); if(l~/;/ && l!~/static_assert|static constexpr|typedef|using |\(/) c++} END{print c}' apps/harmony_timeline.h apps/patcher_abi.h apps/shared_memory.h` returns 66. Rust: `awk '/^#\[repr\(C/{r=1;next} r&&/^pub struct/{inb=1;r=0;next} inb&&/^}/{inb=0;next} inb&&/^[ \t]+(pub )?[A-Za-z_][A-Za-z0-9_]*[ \t]*:/{c++} END{print c}' patcher_rust/src/lib.rs` returns 65. The PER-TYPE breakdown is commanded too, because a total that
agrees says nothing about where a difference sits: C++ `awk '/^struct (alignas\([0-9]+\) )?(HarmonyEvent|MusicalLogicPayload|PatcherEuclideanConfig|PatcherSliceSelectConfig|PatcherRandomDegreeConfig|PatcherLfoConfig|PatcherContext|EventEntry) \{/{n=$NF=="{"?$(NF-1):$NF;inb=1;c=0;next} inb&&/^};/{print n,c;inb=0;next} inb{l=$0;sub(/\/\/.*/,"",l); if(l~/;/ && l!~/static_assert|static constexpr|typedef|using |\(/) c++}' apps/harmony_timeline.h apps/patcher_abi.h apps/shared_memory.h` returns 8, Rust `awk '/^#\[repr\(C/{r=1;next} r&&/^pub struct/{n=$3;inb=1;c=0;r=0;next} inb&&/^}/{print n,c;inb=0;next} inb&&/^[ \t]+(pub )?[A-Za-z_][A-Za-z0-9_]*[ \t]*:/{c++}' patcher_rust/src/lib.rs` returns 8, and
the two outputs are the block below. A total is a sum; the claim that the difference is one type is
about the summands, so the summands carry the commands.

    MEMBERS PER TYPE (cpp/rust)
    EventEntry 7/6
    HarmonyEvent 4/4
    MusicalLogicPayload 9/9
    PatcherContext 26/26
    PatcherEuclideanConfig 9/9
    PatcherLfoConfig 4/4
    PatcherRandomDegreeConfig 4/4
    PatcherSliceSelectConfig 3/3

`PatcherContext` is `patcher_abi.h:114-152` / `lib.rs:109-143`. The whole 66-vs-65 difference
isolates to ONE type, and the commands corroborate the one-sided member below instead of merely
agreeing with it. The eight C++ declarations live in three headers
(`harmony_timeline.h`, `patcher_abi.h`, `shared_memory.h`), which is why a single-file recipe could
not have reproduced this. **Floor, and it is a real one:** both commands count field-like
DECLARATIONS by line shape, so they are format-sensitive — two members declared on one line, or one
wrapped across two, changes the count without changing the ABI. The independent evidence run records
this as `PASS_AT_PIN_FORMAT_SENSITIVE`. The figures hold at this pin and are not robust to
reformatting, which is the difference between a command that reproduces a number and a command that
would survive an edit to the source. *One-sided members* — exactly 1 [HAND-CLASSIFIED — open item 25 (all)], corroborated independently by the per-type block above, whose only row with unequal sides is `EventEntry 7/6`: C++ `EventEntry::ready`, offset 60, size 4
(`apps/shared_memory.h:450`, offset pinned at `:463`).

**Floor.** Four floors. The type population is blind if either method's count moves without the other:
bindgen's file closure and the `#[repr(C` grep must AGREE, and **the grep is invalidated the moment a
declaring macro is introduced** — which the proposed mechanism does, so the gate goes red on its own
floor unless the population derivation moves with it. Member counts are floors of 66/65: a member
added inside existing padding does not move `sizeof`.

**Failure model.** (1) Same-size type substitution — the class a size/offset/name tuple cannot see.
(2) One-sided member pinning, on the only type that has any: `HarmonyEvent`'s four offsets are
asserted in C++ and not in Rust. (3) Config-struct reorder, invisible to any size check: `node_config`
is a `const void*` (`patcher_abi.h:130`) and the sole admission test at each of four consumers is a
size floor.

**Deterministic test.** No engine, no audio device, no shared memory, no plugin host, no sleep. **M1**
the C++ numbers are GENERATED by bindgen over `apps/patcher_abi.h` in `patcher_rust/build.rs`, not
typed. **M2** the Rust numbers come from `core::mem::offset_of!`/`size_of` over the mirrors. **M3** the
comparison gates the artifact `daw_engine` links. **M4** a C++-only edit re-runs the comparison on
BOTH the cargo leg and the cmake leg. A mutation battery drives single-sided edits.

**PASS conditions.**
1. One generated set of numbers, not two hand-written ones. *REFUTED BY* the check passing with the
   C++ numbers supplied by a checked-in table.
2. The comparison gates the linked artifact. *REFUTED BY* a Rust-side-only drift (swap `lib.rs:71-72`)
   that still links after `rm -f` of the staticlib and a rebuild.
3. A C++-only edit re-runs the comparison — cargo leg. *REFUTED BY* widening `uint32_t num_frames` to
   `uint64_t` at `patcher_abi.h:120` and `cargo build` succeeding.
4. Same edit — cmake leg. *REFUTED BY* `cmake --build` succeeding, or a build log containing no
   `cargo` invocation. This is what today's wiring does.
5. Member-level comparison including type form. *REFUTED BY* `float sample_rate` → `uint32_t` at
   `patcher_abi.h:118` exiting 0, when nothing moves, nothing resizes and no name changes.
6. The type population is derived by two independent methods that must agree. *REFUTED BY* the two
   returning different sets, or by either being a literal list in the check's own sources.
7. Member counts derived, printed and ratcheted per type. *REFUTED BY* a count changing with no
   corresponding declaration change.
8. The one permitted one-sided member is admitted by byte-disjointness and **counted PER TYPE** — **AMENDED BY R14 for `EventEntry`, where the admission does NOT apply**: `ready` is live data on the C++ side and the Rust store's width includes the padding, so parity is required there and the exception survives only for members neither side touches.
   *REFUTED BY* a deletion and a padding-resident insertion in the same commit cancelling — which a
   single global integer permits, and which the predecessor's version permitted.
9. Names join under a pure rule with no alias table, and the gate is RED at this SHA until a rename
   lands — the rename itself is open item 24 (G0-B), because a gate that declares itself RED and
   registers no work to clear it reads as a permanent condition rather than a task. *REFUTED BY* the
   check exiting 0 with `patcher_abi.h:75 reserved` and `lib.rs:86 _pad0` both unchanged.

**Static checks.** S-1 the generated table's include sits above the first export at `patcher_rust/src/lib.rs:5` (`pub const PATCHER_ABI_VERSION`); the predecessor anchored this to `lib.rs:893`, which is the `#[cfg(test)]` line 892 lines BELOW that export, so the check it stated would have passed for an include placed under every export in the file. A
position invisible from any run's output. S-2 the check's non-generated sources contain no literal
list of the eight type names. S-3 the one-sided admission is a byte-range disjointness predicate, with
no member name used as an admission key. S-4 name normalisation is a pure function of the input
string — no map or table literal keyed by a member name.

**Review register.** The reviewer SHALL CHOOSE the mutation floor: 600 is a
fifth pinned integer with no owner, and no ticket can be written against a number nobody has picked
(open item 4 (G0-B)). The reviewer SHALL confirm: that libclang's layout for the eight types equals
that of the compiler building `engine_core`, since the C++ numbers come from bindgen and the product
comes from the host compiler; **whether Rust's whole-struct store writes `EventEntry` bytes [60,64), the C++ `ready` member —
STILL OPEN, and my ANSWER TO IT WAS INVALID.** I marked this ANSWERED-NO by reading
`ui/daw-bridge/src/layout.rs:714`, which declares `ready` explicitly, and concluded there was no
overlap. **That is not the endpoint this gate governs.** G0-B's population compares
`apps/shared_memory.h` against `patcher_rust/src/lib.rs`, and `lib.rs:97-105` is
`#[repr(C, align(64))]` with SIX members summing to 60 bytes — **no `ready` field, tail padding at
[60,64)** — while `push_event` at `:158` does `*slot = entry`, a whole-object write. The overlap the
register asked about is exactly there, and the evidence now points AT the hazard rather than away
from it. codex-worker-1 caught the substitution. **I answered a question about one file by
measuring another and published it as resolved** — silently changing the endpoints of a comparison
is worse than getting a number wrong, because the answer looks derived and its subject is not the
one asked about. The register entry stands OPEN and G0-B stays blocked on it; the ruling on the
`PatcherSliceSelectConfig` name collision, the gate being RED until one side renames; and that no ABI
type is passed through `node_config` while declared outside `apps/patcher_abi.h`, which the file
closure cannot see.

---

# G1-A — Ring reservation, publication and reclamation

**Severity** MEDIUM/MEDIUM-HIGH composite. **Dependencies** G0-A.

**Invariant.** A slot in a `RingHeader`-fronted ring carries `ready == 1` if and only if the entry in
it was written in full by the producer that most recently reserved it; no consumer may read a slot's
bytes as data, or release the slot to the producers, in any other state. **Five obligations, not
three** — the predecessor's three did not cover its own failure model, and `ringSkipStalledSlot`
CONFORMS to obligation (b) while committing the defect.

**Population.** Every count is stated as RAW (what the command returns) → RULE (the classification)
→ IN SCOPE, because a bare figure that is a hand-classified subset of its own command is not
reproducible, and four populations in the predecessor were exactly that.
- *Rings* — RAW 17 (`grep -rnF '>capacity' apps/ | grep '=' | grep -v '=='`) → minus 9 non-ring and
  test-fixture assignments → **8**. ⟂
- *Statements interpreting an entry's bytes as data* — RAW 23 EXPRESSIONS
  (`grep -roF 'entries[' apps/`). The line census `grep -rnF` returns 21 because
  `plugin_cache.cpp:460` and `:492` each carry TWO accesses, and this population is named in
  STATEMENTS, so the unit is the expression and not the line.
  → minus 19 (fourteen plugin-cache index sites, which are a DIFFERENT `entries` array, and five ready-flag
  operations, which touch a synchronisation field rather than the entry's data) → **4** ⟂:
  `event_ring.cpp:95` (`entries[write] = staged`), `:116` and `:163` (`entry = entries[read]`), and
  `device_chain_ui_live_tests_main.cpp:112`, which is a test — so **3 production**. The five excluded
  flag operations are `:96`, `:109`, `:126`, `:138`, `:149`, all `storeReady`/`loadReady` on
  `.ready`. A predecessor of this bullet subtracted sixteen rather than
  seventeen and reached five: the twelve plugin-cache sites were right and spread over six files, but
  it counted four non-data operations where there are five. The superseded arithmetic is described
  here rather than quoted, because a retired rule written verbatim inside a claim's own span cannot
  be told from the live one by anything reading the text — which is how it survived a checker that
  was reading for exactly this. The self-check verified 21 − 16 == 5 and could not
  see it, which is why A.0 says in terms that it decides a RULE's arithmetic and not its
  justification. The members are named here so the classification is auditable without re-running the
  grep — and the rule now also ships INSIDE a command, which is what item 6 (G1-A) asked for while it was open:
  `grep -rnF 'entries[' apps/ | grep -v -e pluginCache -e cache.entries | grep -v -e storeReady -e loadReady`
  returns 4, and appending `| grep -v _tests_main` returns 3. A rule stated beside a command is a
  claim about a classification; a rule stated inside one is the classification.
  The ring filter is in the rule, not implied: without it this population measures the wrong set.
- *Read-cursor stores* — RAW **14** (`grep -rn -e readIndex.store -e read_index.store apps/ ui/`) → minus 10 non-ring and test stores → **4**. ⟂ The predecessor printed this command beside the figure 4, which
  the command does not produce.
- *Plugin-cache index sites, which are NOT ring sites* — RAW 23 (`grep -roF 'entries[' apps/`)
  → minus 9 event-ring statements → **14**. ⟂ The unit is the expression throughout this gate; the line count is not restated because a
  second unit beside the first is what produced the 21-versus-23 error. The fourteen are the
  plugin-cache reads and are NOT the population PASS 7 and S4 range over; the RING index sites are
  the separate authored population below. The twelve are: `plugin_cache.cpp:388/:460/:469/:478/:492`,
  `engine_chain_commands.cpp:81`, `engine_save_project.cpp:265/:362`, `daw_engine_main.cpp:291/:1078`,
  and two in `_tests_main` files. The two populations partition the same 23 EXPRESSIONS and are
  stated so the partition is visible: 14 + 9 = 23, and the 9 split 4 data / 5 flag. The line census
  said 12 + 9 = 21 and was a partition of LINES, which is a different set from the sites the gate
  governs — `grep` counts lines, and two of these lines carry two accesses each.
- *RING index sites, the population PASS 7 and S4 actually range over* — AUTHORED under R1, and
  **CROSS-LANGUAGE**. [HAND-CLASSIFIED — open item 25 (all)] An authored population IS a hand-classified one; creating it and leaving
  the exception count at five would have hidden the sixth inside the fix for the fifth. This gate governs a ring that both sides of the SHM boundary index, and every
  earlier version of this population searched `apps/` only, so the Rust half was not omitted by a
  rule — it was never in view. **C++ production (8):** `event_ring.cpp:95`, `:96`, `:109`, `:116`,
  `:126`, `:138`, `:149`, `:163`. **Rust production (3):** `ui/daw-bridge/src/control.rs:454`
  (`read_volatile(ring.entries.add(..))`), `:1465` (`*ring.entries.add(read)`), `:1941`
  (`ring.entries.add(write)`). **11 production**, plus one C++ test
  (`device_chain_ui_live_tests_main.cpp:112`) and one Rust BASE-POINTER construction
  (`control.rs:1969`, `entries.add(entries_offset) as *mut EventEntry`) which indexes nothing and is
  excluded by name rather than by rule. **Drift detectors:** `grep -roF 'entries[' apps/` returns 23
  and `git grep -n 'entries.add' -- ui` returns 4; either figure changing invalidates this authored
  list until it is re-authored. A single-language census of a two-language boundary is not a floor,
  it is a different population wearing the gate's name.

**Floor.** All four are token greps and blind in the same four ways: a cursor mutated through a
whole-`RingHeader` memcpy, a `reinterpret_cast`, a helper that takes the header by reference, or a
macro. The counts are floors, not certainties.

**Failure model.** (1) Torn diff, whose lap-1 consequence is LOSS not store corruption. (2) Stale
`ready` is invisible until the next lap: `drain_ui_out` advances `read_index` without clearing
`ready`. (3) The drain's acquire synchronises with the wrong store — `control.rs:1462` — which is why
a reviewer will believe the drain is fine. (4) `ringSkipStalledSlot` discards a slot that became ready
between the stall observation and the skip. (5) ABA after a full lap, **reachable only on the
multi-producer rings**: `keyRing` has one writer and `ringUiEdit` has none, so ABA is unreachable on
both 64-slot rings — the predecessor's severity sentence is corrected here.

**Deterministic test.** Fixture A is a `#[cfg(test)] mod tests` INSIDE
`ui/daw-bridge/src/control.rs`, with no engine, no shared memory and no threads, because nothing in
the bridge's public API can assert "slot k's `ready` is 0 after the drain". Barriers replace timing at
every transition. The lap fixture stores `(k+1) & mask`; the predecessor stored `write_index = 8` on a
mask-7 ring — a state the producer at `apps/event_ring.cpp:79` cannot create — and read one entry past
the array.

**PASS conditions.**
1. Populations re-derived AFTER the change, not copied. *REFUTED BY* any command printed in this
   gate's Population returning a member this packet does not name. The bullet said "the four
   commands" and this gate no longer has four — the number was a twin of a list that had changed
   under it, so the bullet quantifies over the list instead of counting it.
2. Read guard in `drain_ui_out` on the slot's own flag, acquire ordering, stop at zero. *REFUTED BY*
   the drain returning 1 for an unpublished slot, or `read_index` moving.
3. Disarm on delivery asserted as a **1→0 transition** in the test-owned buffer. *REFUTED BY*
   `entries[0].ready != 0` after a drain that returned 1 — the pin never writes `ready`.
4. The half-fix is rejected. *REFUTED BY* the second lap returning entries.
5. Discard is conditional: `ringSkipStalledSlot` leaves cursor and entry untouched when the slot
   became ready. *REFUTED BY* the retirement being indistinguishable from a discard.
6. Order and ordering decided on the source. *REFUTED BY* S1–S3 passing on the pinned file.
7. **CORRECTED AND NARROWED — the twelve are NOT ring index sites.** This bullet required mask
   discipline at "all twelve sites", and the twelve are plugin-cache reads: bounds-checked accesses
   into `pluginCache.entries`, a `std::vector`, guarded by `device.hostSlotIndex < pluginCache.entries.size()` at `engine_chain_commands.cpp:80-81`. Applying ring-mask semantics to
   them would be a DEFECT, not a discipline: masking a bounds-checked vector index silently aliases
   one plugin's entry onto another instead of refusing an out-of-range slot. The bullet as written
   would have made a correct site fail review and, if implemented, corrupted the chain. It now
   ranges over the RING index sites only, and the plugin-cache reads are named as excluded.
   *REFUTED BY* any RING site whose index operand is neither a mask
   expression nor a local all of whose assignments are.
8. Reservation values do not recur after a lap, on both sides. *REFUTED BY* the raw write cursor
   returning to its pre-lap value.
9. The unguarded C++ reader at `apps/device_chain_ui_live_tests_main.cpp:110-113` — raw index, no
   `ready` load, on a ring with six live producers — is closed. *REFUTED BY* that site still reading
   `entry.type` with no preceding acquire load of that slot's `ready`, which the S2 predicate decides
   on the source; a runtime observation cannot, because an unpublished slot's bytes are
   indistinguishable from a published slot's when the producer has not yet written.

**Static checks.** S1 the ready-clear lexically precedes the cursor store at all four sites, same
function, no return or branch between. S2 the read guard's load is an acquire load of THAT SLOT's
`ready`, and the clear is a release. S3 the re-check and the advance are one step in the callee. S4
mask-in-expression at the RING index sites, explicitly NOT at the twelve plugin-cache reads — see
PASS 7, where the same error appeared and would have corrupted a bounds-checked vector access.

**Review register.** The reviewer SHALL decide whether a producer publishing into a slot it no
longer owns after the 2-second grace is in scope. The reviewer SHALL decide whether to establish or
explicitly decline the producer census on `ui_out` — **this read "SHALL establish or explicitly
decline", a choice wearing a verb the rule does not recognise**, and it emitted as part of the
sentence above it. The reviewer SHALL confirm no second consumer of `ui_out` exists, since the
disarm is safe only under the single-consumer contract; and SHALL confirm Fixture A compiles,
nothing in the pin having been built.

**Correction carried.** The claim of "uninitialised `EventEntry` / indeterminate flags" is withdrawn:
`EventEntry` has an NSDMI.

---

# G1-B — Coherent snapshot and response publication

**Severity** HIGH; the weight is sub-claim (C). **Dependencies** G0-A, G1-A, and the production atomic
size/alignment assertions.

**Invariant.** Every answer a client takes out of the UI segment must be (i) matched to the client's
own question by a **sender-minted** identity written into the request and echoed back verbatim by the
engine — not by the engine's version counter, not by non-emptiness, and not by the engine re-stating
the address the client asked about; and (ii) a complete image of exactly one execution of the writer,
never fields from publication N+1 beside fields from N.

**Population.** *Seqlock opens and closes* — 4 + 4. Command: `grep -rn 'seq\.store' apps` returns 8, cross-checked per file with grep's count mode. *Request/answer readers* — **AUTHORED under R1: 7, by a predicate enumerated from the REQUEST.**
Both earlier attempts enumerated from the wrong side and each missed a member for a structural
reason, so the rule is stated as the correction of that: **the authoritative request set is the
`Request*` values of the `UiCommandType` enum** — numbered members, which a spelling or a helper
convention cannot hide — and a reader is a REQUEST/ANSWER reader iff it reads the REGION one of
those commands publishes. Census: `grep -cE '^\s+Request[A-Za-z]+\s*=' apps/event_payloads.h`
returns 7.

    RequestClipWindow      = 30  →  read_clip_window
    RequestDeviceParams    = 40  →  read_device_params
    RequestWaveform        = 44  →  read_waveform_slot
    RequestAutomationLane  = 62  →  read_automation_slot
    RequestSamplerKit      = 75  →  read_sampler_kit_slot
    RequestSamplerEnvelope = 97  →  read_sampler_envelope_slot
    RequestChainSnapshot   = 37  →  drain_ui_out  — answered by `UiChainDiffPayload` on the UI-OUT
                                    ring, NO request identity, 0..N diffs. IN the population.

**SEVEN. The population is the seven requests and their answers, and the seventh is the WORST
CASE, not an exclusion.** I first wrote "reads the REGION one publishes" and excluded
`RequestChainSnapshot` because its answer travels by ring. codex-worker-1 blocked that and is right:
filtering by TRANSPORT drops exactly the member whose answer is least correlatable, and G1-B's
invariant governs every answer to a UI request regardless of how it travels.
Measured: `handleRequestChainSnapshot` emits `UiChainDiffPayload` into the UI-OUT ring, consumed by
`drain_ui_out`. That payload's fields are `diffType, flags, trackId, chainVersion, deviceId, deviceKind, position, patcherNodeId, hostSlotIndex` — **no sender request identity of any kind** —
and the handler emits ZERO OR MANY diffs (`payload.trackId == 0xFFFFFFFFu` fans out to every track,
an unknown track yields none). An answer with no request identity and no fixed cardinality is the
sharpest instance of this gate's subject, and my rule had it outside the population.

**THAT IS THE SECOND TIME ONE OF MY PREDICATES EXCLUDED THE WORST MEMBER, BY THE SAME MECHANISM.**
The `requestSeq` rule keyed on a correlation TOKEN and dropped `read_device_params`, which has none.
The region rule keyed on a correlation TRANSPORT and dropped the chain snapshot, which has neither
token nor region. **Any predicate keyed on the mechanism that PROVIDES correlation excludes exactly
the members that lack correlation — and those are the defects.** Enumerating from the request was
right; filtering the answers by how they arrive was the residual form of the same error.

**THE SECOND SIGNAL IS OF A DIFFERENT KIND, which is the only sort that counts here.** The seven
`Request*` members are mirrored in `ui/daw-bridge/src/layout.rs` with identical names and identical
values, and the two declarations are checked by two different compilers. That is not a second grep
over the same text — it is the same population expressed twice in a form the build enforces. Every
predicate that failed on this population failed as a text pattern; this one is anchored to a
declaration that cannot be missing or misspelt, because its absence is a build error.
**The general form, which is narrower and more useful than "use the enum": enumerate from the
artifact that CANNOT BE ABSENT.** A correlation token can be missing — that absence is the defect.
A helper function can be missing — `RequestDeviceParams` has none. A label can be spelt `S-1` where
every sibling writes `S1`. An enum member is present or the program does not build.

**WHY TWO EARLIER PREDICATES FAILED, recorded because the failures are the argument for this one.**
A rule selecting on a CORRELATION TOKEN (`requestSeq`) is circular — it cannot see a request/answer
reader that LACKS one, and the absent token is the defect this gate exists to find; it missed
`read_device_params` and `read_clip_window`. A rule selecting on a HELPER FUNCTION NAME
(`send_*_request`, 5 of them) missed `read_device_params`, whose request is a COMMAND on the ring
with no helper. Both enumerated from the ANSWER side or the CALLER side. The request is an enum
value; enumerating from there cannot lose a member for lacking a token or a helper.

**THE GATE-RELEVANT PARTITION, measured by claude-worker-1 and verified independently here — total
over all 21 candidates, over a property this gate is about:** does the reader guard against a torn
read at all? **17** seqlock-guarded (loop, `v0 == v1`, Acquire fence) · **1** forwarder
(`read_clip_extents`, whose guard is in the callee `read_clip_extents_with_truncation`) · **3** with
NO GUARD: `read_scales`, `read_device_params`, `read_device_meters`.
`read_device_params` is in BOTH the request/answer seven and the unguarded three, which is exactly the
intersection the failure model names — daw-agent "reads the region before waiting at all", and it is
unguarded on top. The other two unguarded readers are reported with it rather than dropped, because
handing over only the member that fits the story is how a population becomes an argument.

**Drift detectors, SPECIFIED here and not yet IMPLEMENTED as a static check:** the `Request*` enum
census returns 7 and `grep -c 'pub fn read_' ui/daw-bridge/src/control.rs` returns 21; either
changing invalidates this authored list until re-authored. **One word was doing two jobs** — a
detector's command and figure, which are stated, and a runnable check that asserts them, which is
item 33 (G1-B)'s — so R1 read as "the detectors exist" against S4's "they are not written" and both
were true of different things. backend and codex-worker-1 both filed the contradiction.

**Item 23 (all) is RESOLVED BY THIS RULE, not by it excluding the question:** `read_clip_window` IS a
request/answer reader — `RequestClipWindow` is command 30 — and the exact review's classification was
right. It lacks a per-slot correlation token and is guarded by the global `ui_version`, which is why
a token-based rule could not see it.

**Floor.** Four blind spots, each with its count: `seq.store` == 8 at pin, and a whole-header memcpy
or a helper taking the region by reference is invisible to it; `pub fn read_` == 21; the call-site
extraction cannot see a reader reached through a trait object. **The by-hand selection of 6 from 21 is
the weakest link and is named as such** rather than presented as a derivation.

**Failure model.** (1) Same address, different device, **no timing involved**: daw-agent's
`device_params` reads the region before waiting at all (`ui/daw-agent/src/tools.rs:1614`). (2) The
consequence stated at the strength the code supports — a stale answer, not necessarily an audible
wrong write; the predecessor overstated it. (3) Delay turns into a wrong answer rather than a timeout,
because `requestPluginParams` is a blocking round trip held under `controllerMutex`. (4) Seqlock open
without a release fence before the first payload store — genuinely reorderable on the arm64 machine
this is built on. (5) In-place rewrite with no in-flight marker and a reader with no double-check.

**Deterministic test.** Fixture 1 swaps a device behind a reused id with the engine structurally
unable to republish, deciding (C) against a live engine with **no timing dependence, because there is
no answer in flight to race**. One engine on a private `DAW_UI_SHM_NAME`; track 0 chain = two
resolvable VSTs; the second is removed and the id reused. Fixtures 2–4 drive the seqlock and minting
halves with barrier-paused writers.

**PASS conditions.**
1. The four device-params consumers accept only on a sender-minted echo. *REFUTED BY* a consumer
   accepting on the engine-minted `version`, on non-emptiness, or on the engine restating (track,
   device).
2. Fixture 1 step 3 reports failure and step 1 reports success. *REFUTED BY* a params array present in
   step 3 — decisively if it carries the removed plugin's uid16 values — or an error in step 1.
3. No minting expression maps two questions to one identity. *REFUTED BY* fixture 4a producing a
   repeated value, which `NEXT.fetch_add(1, AcqRel) | 1` does for counter values 2 and 3
   (`ui/daw-agent/src/tools.rs:1671`).
4. The four seqlock opens each carry a release fence between the odd store and the first payload
   store, decided on the source AND the emitted code, never on an execution. *REFUTED BY* the fence
   being absent at any of the four.
5. The two in-place rewrites carry an in-flight marker for the whole rewrite and their readers consult
   it. *REFUTED BY* a reader accepting an image written during the rewrite.
6. Fixtures 2c and 2f are refused; 2b and 2d accepted. *REFUTED BY* `read_device_params` returning a
   populated view for 2f — the image carrying `trackId`/`deviceId` from
   `apps/engine_request_commands.cpp:122-123` beside a stale `paramCount`.
7. Neither of the two named readers loads a peer-written word through a plain non-atomic load. The
   universal form ("no reader ...") is still NOT used, but the reason has CHANGED and this sentence
   outlived the change: it said the universal quantifies over a WITHDRAWN population, and R1
   authored that population — item 11 (G1-B) is CLOSED. The universal remains unused now only
   because the authored population is SEVEN and this bullet is about two of them; a bullet naming
   its two sites is decidable where "no reader" would require re-deciding all seven at every run.
   **This sentence has now been wrong in both directions.** It said "seven members" when the section
   heading said six; I resolved that by trusting the HEADING and wrote six — and the heading was the
   stale one. The body states seven and gives the argument: `RequestChainSnapshot` is IN the
   population, answered by `drain_ui_out` with no request identity, and it is the WORST CASE rather
   than an exclusion. **Resolving a contradiction by picking the shorter statement is not resolving
   it**; the heading is now seven, which is what the reasoning under it always said. Both named sites fail today
   and the bullet names both: `ui/daw-bridge/src/control.rs:944-949`. Quantified over THOSE TWO
   SITES, and the phrase "two of the six" would have been wrong twice over: the hand-selected six
   were withdrawn, and what replaced them under R1 is seven. A bullet naming its sites survives a
   population being re-authored underneath it, which is why for the same reason the population is. Whether two is the whole set of
   offending sites was item 11 (G1-B), now CLOSED; that these two offend is decidable at this SHA and is
   what the bullet asserts. *REFUTED BY* either remaining.
8. An abandoned publication is distinguishable from an unanswered one within a bounded number of
   observations, and the reader reports which it saw. *REFUTED BY* a reader that returns the same
   value for both. This replaces a predecessor bullet requiring only that the condition be
   "observable", which any implementation satisfies.
9. **RESTORED — the population it waited for exists.** This bullet was withdrawn because a
   mechanical echo ratchet needs a set to range over and the set was withdrawn; R1 authored it and
   item 11 (G1-B) is CLOSED, so the condition it was waiting on is met and the withdrawal outlived
   it. **The requirement is a ratchet over the SEND SITES, and that population has never been
   authored** — what R1 authored is the request kinds (seven, from the enum) and their answers
   (seven — six region readers plus `drain_ui_out`, which the population INCLUDES as its worst case
   rather than excluding; this clause said "readers (six)" and took the six-member count from
   before that correction); the sites that SEND a request are a third population and nobody has
   enumerated them. So
   this bullet is blocked on a missing POPULATION and not merely on an unwritten test, which is a
   different and larger gap than the one I recorded last SHA when I wrote "the seven authored send
   sites" — a phrase that took a count from one population and a unit from another. Carried by open
   item 33 (G1-B). *REFUTED BY* adding an eighth send site to the authored
   population and observing the ratchet stay green — which is what it does today, there being no
   ratchet, so this bullet is refuted at this SHA and says so rather than reading as met. Leaving it standing would have been a
   PASS condition satisfiable by ratcheting the empty set.

**Static checks.** S1 fence between open and first payload store — statement ORDER is invisible from
any return value, since a clean image proves nothing about the fence. S2 write order inside the
device-params body: identity words first (`:122-123`), `paramCount` last (`:169`), which is what makes
the stale answer readable as complete. S3 minting expressions, naming the two collapsing forms. S4
**RESTORED — the population it waited for exists.** It required drift detectors asserting "the
three counts", which were counts over the hand-selected six the packet withdrew; a drift detector
over a withdrawn population detects drift in nothing. R1 authored the replacement and item 11 (G1-B)
is CLOSED, so the detectors now have a set to pin. **Their commands and figures are SPECIFIED in
G1-B's population; what is not written is a runnable check asserting them**, which makes this an
open test rather than a withdrawn one — the distinction the whole gate turns on. The requirement it
encodes — that the authored population must have detectors pinning its size — is
carried by open item 33 (G1-B) rather than left as an unrooted static check here. This paragraph
also said the check returns with item 11 (G1-B), three lines after saying item 11 is CLOSED; one
paragraph naming two carriers, one of them since discharged, is the restatement defect at its shortest
range.

**Review register.** The reviewer SHALL rule on how device-params acquires a request identity — a new
field on `UiDeviceParamsRegion` implies a `kShmVersion` bump. The reviewer SHALL decide what
abandonment this gate defends against — process death, an exception inside the body, or neither —
since there is no early return between any open and its close. **Each choice is its own sentence, so
each gets its own record**; they were one sentence and the second had none. The reviewer SHALL
confirm whether the plain snapshot copy at `control.rs:644-646` bracketed by the `ui_version` double
check is acceptable; and SHALL state which thread owns each region's publication and confirm the
device-params region and the audio clip table are INTENDED to be single-writer.

---

# G2-A — End-to-end command identity

**Severity** MEDIUM-HIGH. **Dependencies** G1-A, G1-B.

**Wording of record.** The defect is NOT that a caller is told a different command was applied. It is
that a caller **adopts another command's terminal outcome** for its own, because correlation is by
`(scope, command, base)` with no command id.

**Invariant.** A client's terminal verdict on a mutating command may be adopted from an
engine-published record only when that record carries an identity **the sending process minted** for
that one command instance and the engine copied through unchanged. The mechanism is named so an
engine-side discriminator cannot empty the clause: a correlator presented with a record whose every
content field equals its own command, but whose identity it did not mint, must return not-mine.

**Population.** **AUTHORED under R12 — the three production adoptable-refusal emit sites; see the
ruling for the derivation and for why the arbitration sites are ROUTES rather than members.** The
earlier retraction is lifted. My claim that B = A ∪ {SetRowOps} was wrong, and its first stated
reason was wrong too; the corrected mechanism is this.** THERE ARE TWO FUNCTIONS NAMED
`requireMatchingClipVersion`: the free one at `clip_edit.cpp:5-15`, which sets
`UiDiffType::ResyncNeeded` and nothing else, and the engine one at `engine_clip_edit.cpp:932-997`,
which emits **BOTH** — `emitUiDiff(diffPayload)` at `:982` carrying ResyncNeeded AND
`emitClipReject` at `:965` (UnknownTrack) and `:986` (StaleBase). I read the first and wrote that
the arbitrated path emits ResyncNeeded and not ClipRejected. `event_payloads.h:1469-1473` states the
design in the product's own words: "ResyncNeeded (4) is still emitted alongside, unchanged... This
is strictly additive."
**The two reviewers' findings reconcile once the harmony arbiter is separated from the clip one.**
`requireMatchingHarmonyVersion` (`engine_harmony_timeline.cpp:111-128`) emits a harmony diff ONLY —
no ClipRejected. So CLIP-arbitrated commands emit both refusals, HARMONY-arbitrated commands emit
only ResyncNeeded, and `SetRowOps` emits ClipRejected while being unarbitrated. That gives
B = (A − {WriteHarmony, DeleteHarmony}) ∪ {SetRowOps}: neither set contains the other, which is
claude-worker-1's original NOT-NESTED, reached by codex-worker-1's route.
**A control built on "ClipRejected means unarbitrated" would classify every StaleBase refusal — the
arbitrated ones — as unarbitrated**, which is the retracted containment error mirrored.
claude-worker-1 told me these populations were NOT NESTED and I replaced that with a containment
claim I had not verified — a correct finding overwritten by a tidier wrong one, which is worse than
never having had it. The review also reports that the subtraction reaching 9 gives 8 command-facing
sites representing 9 command types, with `engine_clip_edit.cpp:971` an internal helper I miscounted
— stated without repeating the raw figure, which is claimed once in the population above.
**Superseded scope**Superseded scope, retained as the record of what was falsified:**
`engine_rowops_commands.cpp:49` emits `emitClipReject(..., daw::UiCommandType::SetRowOps)` from a
true-mutating arm, and `SetRowOps` is not among the nine, so "the only commands that can produce an
adoptable refusal" is false as written. The 51 correlator call sites compound it: they correlate
chain, sampler, mod-link and journal refusals too, so the correlator population and the
arbitrated-command scope are different sets that this gate treated as one. The figures below are
retained as COUNTS and neither may be read as a scope until item 27 settles what the gate governs.
The nine were: `WriteNote`, `DeleteNote`, `WriteChord`, `DeleteChord`, `RevertPlacementOverrides`,
`SetAutomationTarget`, `SetClipText`, `WriteHarmony`, `DeleteHarmony`. RAW 17
(`grep -rn -e 'requireMatchingClipVersion(' -e 'requireMatchingHarmonyVersion(' apps | grep -v tests_main`)
→ minus 8 (three definitions, three declarations, and the two `daw_engine_main.cpp` lambda forwarders
at `:1739` and `:1744`) → **9 call sites**. ⟂ The predecessor described this command in prose instead of
printing one, so its 9 could not be re-run. The nine COMMANDS map onto the nine sites non-bijectively
— the chord site carries both `WriteChord` and `DeleteChord` through a runtime-computed type — so the
correspondence is established by the Floor paragraph below, not by this arithmetic.
`commandMutatesDocument` (`apps/engine_command_mutates.h:46-236`) has **93** case arms — **68**
returning true and 25 returning false. A raw count of `return true` gives 69; the 69th is at `:235`,
AFTER the switch closes at `:234`, and is the unreachable post-switch fallback for a hostile cast,
not an arm. Previously stated as 69 returning
true — the predecessor said 186, off by 2× in the section whose credibility rests on counts — but for
the other ~60 families no `ClipRejected` is ever emitted, so an identity obligation over them would
require inventing emit sites the code says do not exist. *Terminal refusal records* — 3 `emitClipReject` sites
(`git grep -n 'emitClipReject(' -- apps | grep -v -e 'void emitClipReject' -e daw_engine_main.cpp -e _tests_main`
returns 3) plus 2 refusal journal lines. *Correlator call sites* — RAW 56 mentions
(`grep -rn -e await_clip_outcome -e report_outcome_from -e report_refusal_outcome -e refused_or ui/daw-cli/src ui/daw-agent/src`)
→ minus 5 (four `fn` definitions, and the doc comment at `ui/daw-cli/src/main.rs:1262` that names
`await_clip_outcome` in prose) → **51 call sites** ⟂, which is 4 + 6 + 17 + 24 exactly as the exact
review stated. This gate no longer carries that number on attribution: the command above reproduces
it, and re-deriving it is what showed my own competing figure of 56 to be a MENTION count — the
predicate was mine, not an arithmetic error of the reviewer's. The predecessor said 40; my 40 was a subtraction
from a hand-classified list and the reviewer's is a re-derivation. The set includes `refused_or` →
`await_refusal` (`ui/daw-agent/src/tools.rs:62-67`) with 24 call sites, one of them `set_clip_text`
(`:3239`), a governed command — a family the predecessor omitted entirely.

**Floor.** Three, in the model of `tools/version_arbiter_check.sh:69-78` ("finding FEWER means the
extraction broke, not that the code got safer"), which the predecessor cited nowhere. The nine are
**not name-greppable**: the chord site passes a runtime-computed value —
`const auto commandType = payload.commandType == … ? WriteChord : DeleteChord;` then
`requireMatchingClipVersion(chordPayload.baseVersion, commandType, …)`
(`apps/engine_note_commands.cpp:136-142`). Re-running the extraction by hand yields FIVE names against
that script's own floor of `-lt 7`, so the shipped check may already be blind.

**Failure model.** (1) False refusal adopted → re-send → a second undo step: `retry_stale` is the
DEFAULT (`ui/daw-cli/src/main.rs:2979-2981`), and a doc comment at `:1143-1145` records that this
re-send already shipped once and broke redo. (2) The retry cannot observe its own outcome —
deterministic, no concurrency, no second author, and absent from the predecessor. (3) The only defence
is a window position: `before_len` is the LENGTH of a non-consuming peek.

**Deterministic test.** Three layers. A stress run that fails to reproduce adoption is not evidence
adoption cannot happen, and one that reproduces it is not evidence about a fix — so no bullet depends
on a sleep, a load level or scheduling. Layer 1 barriers two clients at V and forces both acceptance
orders with concurrent draining; Layer 2 drives the correlators against a record stream containing a
foreign record; Layer 3 asserts process exit code and stdout, not internal verdicts.

**PASS conditions.**
1. The outcome record carries the id the sender minted, copied and not computed. *REFUTED BY* an id
   the engine derives.
2. The id is unique across sending PROCESSES, not only within one. *REFUTED BY* two processes
   producing the same id — a per-process counter does.
3. No record-reading correlator adopts a foreign record, and each still adopts its own. *REFUTED BY* a
   correlator returning a terminal verdict for a record containing a foreign identity.
4. The counter-only decisions are REPLACED, not stamped. *REFUTED BY* a site still deciding on
   version movement with an id present but unread.
5. The decision rests on identity, not a consumable window position. *REFUTED BY* deleting
   `.skip(before_len)` (`main.rs:1171`) changing any verdict.
6. Every surviving room claim carries a recomputed byte count. **`UiClipRejectPayload` uses 18 of 40
   bytes and has 22 free** — `reserved` (2) plus `reserved2[5]` (20), `apps/event_payloads.h:1570-1578`;
   the predecessor said "uses 22 of 40", inverted. `UiCommandPayload` is exactly 40 with
   `static_assert(sizeof == 40)` at `:2169`, so room THERE requires repurposing a field, not spare
   bytes — a distinction the predecessor blurred. *REFUTED BY* any room claim whose arithmetic does not
   reproduce.
7. The retry is gated on identity, not on the reason code. *REFUTED BY* reaching the re-send on
   `StaleBase` alone.
8. **Unknown is reported distinguishably from Applied**, asserted on the process's exit code and
   stdout. Not an identity bullet, and present because `ClipOutcome::Unknown` is documented "Treated
   as applied" (`main.rs:1155-1158`) and callers return 0 on `Applied | Unknown` (`:2990-2994`), so an
   implementation answering Unknown to everything satisfies 1–7 and preserves the entire harm.
   *REFUTED BY* exit 0 with `{"sent": …}` on the unknown case.
9. The BATCH's base and its wait read the same counter, asserted on the SOURCE. *REFUTED BY* the check
   passing while `resolve_base` (`ui/daw-sidecar/src/main.rs:3277-3289`) keeps the crossing on the
   path a browser transpose takes — the **BATCH blindness** the predecessor's open list dropped.

**Static checks.** S1 the mark is sampled before the send — no `handle` method call inside any
`await_clip_outcome(` argument. S2 the echo is a copy, not a computation, at each of the three emit
sites. There is no S3: the check that stood here ranged over an extraction with no predicate and is
carried by open item 28 (G2-A). **S4 the BATCH's two counter reads resolve to one accessor** —
restored here after the edit that removed S3 deleted S4 with it, while PASS 9 and open item 14 (G2-A)
still depend on the property S4 asserts. Removing a list entry by deleting the text around it is
how a gate loses a check without anyone editing that check.

**Review register.** The reviewer SHALL re-derive the command and correlator populations at the
reviewed SHA and fail the gate if any extraction returns fewer than its floor; SHALL run
`tools/version_arbiter_check.sh` on the live tree and confirm whether its clip extraction returns 5
against its floor of 7, i.e. whether the shipped check is already blind; SHALL confirm no consumer
other than `apps/engine_rt_helpers.cpp:387` reads `EventEntry.flags` on a `UiCommand` entry before any
design commits to those 4 bytes; and SHALL decide whether `WriteHarmony`/`DeleteHarmony` get a refusal
record at all, since at this SHA a refused harmony write publishes only a scope-free `ResyncNeeded`.

---

# G2-B — Recovery readiness and state publication

**Severity** MEDIUM-HIGH. **Dependencies** G0-A, G1-A, G1-B, G2-A.

**Scope, widened from the predecessor.** The gate reaches per-slot **bypass** and the **failure of `sendSetBypass`**.
It does NOT currently reach the **parameter mirror replay**: the bullet that did was withdrawn as
circular (see PASS 4), so the mirror half is specified by the register and open item 18 (G2-B), not by a PASS
condition. The predecessor asserted the mirror half was in scope while its bullet was unsatisfiable, which is
worse than the gap it was closing.

**Invariant, stated in full, with the clause this gate cannot decide marked as such.** OBSERVABLE:
no `ProcessBlock` is dispatched to a host whose per-slot bypass differs from the authored chain,
whose readiness LEVEL is below the level that dispatch
requires, or for which a `sendSetBypass` failed without the readiness being withdrawn. **R2 is
propagated here:** readiness is `mapped-and-bypassed` or `mirror-complete`; a `ProcessBlock` may be
dispatched at `mapped-and-bypassed`, and only processing that DEPENDS on mirrored parameters
requires `mirror-complete`. That is what dissolves the circularity — the ack that establishes
`mirror-complete` arrives during a `ProcessBlock` the lower level already permits, so the gate no
longer demands a state reachable only through a state it forbids. The middle clause is a property of the SYSTEM and belongs in the
invariant; it is not a property this gate can decide at this SHA, because the acknowledgement it
would test for arrives only during a `ProcessBlock` that `processTrack` refuses while `hostReady` is
false. Scope excluding the mirror while the invariant required it was a real contradiction and is
resolved by marking the clause rather than by deleting it — deleting it would make the gate
self-consistent by forgetting the requirement, which is how the predecessor's version came to assert
the mirror half was in scope with an unsatisfiable bullet behind it.
"Authored" means the vector as copied at `apps/daw_engine_main.cpp:1113` under `trackMutex` — that
copy is the only referent that exists, because the guard is released at `:1114` while the `SetBypass`
loop runs at `:1116-1124` under `controllerMutex` alone, so the chain can change across the gap.
MECHANISM: `hostReady` is release-published at `mapped-and-bypassed` once bypass is staged and
acknowledged; the mirror ack raises the level to `mirror-complete` and does NOT gate the initial
publication. Requiring all three acks before publishing was the pre-R2 formulation and is what made
the circularity unbreakable.

**Population.** *`hostReady` publish sites* — RAW 4 (`grep -rn 'hostReady' apps | grep 'store(true'`) → minus 1 in `_tests_main` → **3 production** ⟂: `engine_restart_worker.cpp:87` (the site under gate),
`engine_track_setup.cpp:62` and `engine_track_setup.cpp:403`. All three are named because the
predecessor's "and one other" left a member of an exact population unidentified. The predecessor also
said "exactly 4", counting `engine_track_setup_tests_main.cpp:52`. *`controllerMutex` acquisitions in product code* — 28.
Command: `grep -rnE '(lock_guard|unique_lock)<std::mutex>[^;()]*\([^;]*controllerMutex' apps | grep -v tests_main` returns 28.
*`hostReady` read sites in product code* — exactly 20, `daw_engine_main.cpp:1107` among them. Command:
`grep -rnE 'hostReady(\.|->)load' apps | grep -v tests_main` returns 20.

**Floor.** All three are token greps over `apps/` at this SHA and reproduce from the printed commands.
They are blind to a publish or read reached through a function pointer or a `std::function`, which is
exactly how `applyHostBypassStates` is delivered to the worker — so the publish-site count is a floor
of 4, not a certainty.

**Failure model.** (1) Crash recovery mid-playback, bypass half: the watchdog callback clears
`hostReady` and sets `needsRestart` (`engine_restart_worker.cpp:83-85`), and the publish at `:87`
precedes the recovery hook at `:88`. The window is bounded by two contended mutex acquisitions and a
heap allocation, all AFTER the publish, and the resulting block is mixed to the output. (2) The naive
fix produces the second failure shape: swapping `:87` and `:88` does not narrow the window, it deletes
the recovery, because the hook's own guard reads `hostReady`. (3) Offline render and the master:
`apps/engine_produce_block.cpp:384-387` takes `controllerMutex` BLOCKING when offlineRender, and
`apps/engine_audio_callback.h:317-319` does not, so a try_lock probe cannot cover both.

**Deterministic test.** Stated first: `applyHostBypassStates` is a lambda defined inside `main()` at
`apps/daw_engine_main.cpp:1106-1125` and delivered as a `std::function`, so the fixture reaches it
only through the worker. With the worker provably parked inside the recovery hook at
`engine_restart_worker.cpp:88` — blocked on `probeDone`, holding no test-owned lock — an RT-shaped
probe records a refusal, and a separate offline-shaped probe records the blocking case. **The offline
thread may not attempt its blocking acquisition until the RT probe has appended its verdict**; without
that ordering the PASS token is producible by contention from the packet's own probe, which is the
false-green the predecessor's open list dropped.

**PASS conditions.**
1. The decider contains no clock. *REFUTED BY* any verdict that changes when the probe pacing changes.
2. The blocking shape is covered, which the try_lock probe cannot decide because a try_lock refusal is
   also what contention looks like. *REFUTED BY* the offline probe acquiring during recovery.
3. The swap trap: `hookEntryHostReady` recorded as the hook's first statement must be TRUE.
   *REFUTED BY* a fix that reorders the two lines, which makes it false and deletes the recovery.
4. **WITHDRAWN AS CIRCULAR — see the register.** The predecessor required the mirror to be
   acknowledged before `hostReady` is published. The acknowledgement can only arrive during a
   `ProcessBlock`, and `processTrack` refuses to dispatch while `hostReady` is false, so the condition
   is unsatisfiable by construction: it demands an ack that the state it gates makes impossible. No
   bullet replaces it until R2's two-level readiness is PROPAGATED into this gate — the owner has ruled and the recovery-only priming protocol is the REJECTED alternative; a PASS condition
   that cannot be met by any implementation is worse than an absent one, because it reads as covered.
5. **A `sendSetBypass` failure withdraws readiness.** *REFUTED BY* `hostReady` remaining true after a
   forced send failure.
6. Liveness scoped to the path that reaches the publish. *REFUTED BY* the re-run probe being refused
   after `restartInFlight` goes false.
7. The witness set converts a vacuous green into a FAIL: "recovery-hook-entered" appears exactly once
   and the cycle ends with `hostGaveUp` false. *REFUTED BY* a green run with zero hook entries.
8. The control sabotage restores only the ordering and must flip the decider. *REFUTED BY* the decider
   holding under a build whose publish precedes the recovery.

**Static checks.** S1 publish-site ratchet, exactly the recorded 4 lines. S2 single arming site for
the mirror: `grep -rnE 'mirrorPending(\.|->)store\(true' apps` returns 1, `engine_rt_helpers.cpp:38`.
S3 anti-deadlock: no `mirrorPending|mirrorPrimed|mirrorGateSampleTime` token between
`engine_produce_block.cpp:361` and the lock construction. S4 probe fidelity: `:362` still reads
`hostReady` with acquire and `:385-387` still constructs the blocking lock.

**Review register.** The recovery CONTENT is not mechanically decidable at this SHA: the reviewer
SHALL confirm by reading `apps/daw_engine_main.cpp:1106-1125` that the recovery issues exactly one
`SetBypass` per slot. The reviewer SHALL confirm no artifact describing the fix says "the authored
bypass value" without qualifying it as the copy at `:1113`. The reviewer SHALL confirm, when the fix
lands, that mechanism substitution has not voided the gate. The reviewer SHALL classify any NEW
`hostReady` publish site, which S1 can detect but not judge. **The mirror-ack circularity is RULED (R2: two-level readiness) and the reviewer SHALL confirm the
propagation rather than choose**, which this paragraph asked for before the ruling existed: the ack arrives only during a
`ProcessBlock`, which `processTrack` refuses while `hostReady` is false. **R2 CHOSE: readiness is staged in two levels**
(`mapped-and-bypassed` versus `mirror-complete`) with dispatch permitted at the lower one. The
alternative — a recovery-only priming exemption — is REJECTED and named here so it is not re-proposed
as though the question were open. This paragraph previously said the packet "deliberately does not
choose", which was true before R2 and false after it; the option list is retained as the record of
what was decided against, not as a live choice. The reviewer SHALL also rule on the self-deadlock: the fix class this gate admits requires `applyHostBypassStates` to stop taking
`controllerMutex` at `:1116`, i.e. a caller-holds contract or a lock-passing signature — which the
predecessor did not state and which is a design decision, not a detail.

---

# G3 — Per-host failure containment

**Severity** HIGH; blast radius is the entire session. **STATUS: RULED (R3: N = 3, authored), NOT YET
ACCEPTANCE-DECIDABLE.** The ruling exists; what is missing is the ticket that pins it — N with units
and semantics, a STATIC CHECK ON THE LITERAL (which does not exist at this SHA and is required),
watchdog instrumentation, drift acceptance and independent validation. Describing this gate as
"blocked on an owner ruling" was true when written and is now false. Every PASS condition below is written against a bound N counted in observations,
and nothing in the tree sources N: all three production `Watchdog`s use `hardTimeoutBlocks = 500`,
which is a block count and not an observation count, and no other artifact proposes one. The exact
review's verdict that this gate "remains undecidable" is accepted rather than argued. The bullets are
retained because they are correct GIVEN N, and N IS RULED — R3 authors it at 3, so they are decidable
now. This sentence used to say they "become decidable the moment N is ruled" and that no
implementation could be accepted "until then"; that chronology outlived the ruling it waited for. **Dependencies** G2-B **of this packet** — the ruling on N that this clause used to name as a prerequisite is discharged by R3, which authors N = 3, and item 19 records that no further ruling is required; the clause outlived the ruling and went on emitting a prerequisite the packet had already met, which is why the manifest kept reporting one. Note the parser reads gate identifiers out of this clause, so an all-caps word here is read as a gate id — the
gate as received named a document that does not exist in the pinned tree, and the reference is
resolved here rather than left dangling.

**Invariant.** For a NON-MASTER track — a member of the `tracks` vector snapshotted at
`apps/daw_engine_main.cpp:962-969`, which excludes the master constructed at `:449` — a hosted plugin
that stops advancing its `completedBlockId` while its control socket stays writable must, after a
declared bound N counted in OBSERVATIONS OF THAT HOST, have `hostReady` stored false by an autonomous
engine path, and block production for the remaining tracks must continue.

**Population.** *Tracks whose production must continue* — [HAND-CLASSIFIED — open item 25 (all)] the `tracks` vector, read at
`apps/daw_engine_main.cpp:962-969`. *Writes that remove a host from the gate population* — RAW 17
(`grep -rn 'hostReady' apps/ | grep 'store(false'`) → minus **2** in `_tests_main` → **15
production** → minus 2 master-track stores (`engine_master_render.cpp:121` and `:132`) → **13 IN
SCOPE**. ⟂ This gate's invariant excludes the master bus, so the fifteen came from a wider scope than
the gate governs and two of its members are exactly the case the invariant does not cover. Either
the scope widens or the population is 13; the packet takes 13, because widening a scope is a design
change and correcting a count is not. The
whole census is RAW 21 (`grep -rn 'hostReady' apps/ | grep 'store('`) → minus 3 in `_tests_main` → **18 production**. ⟂ The predecessor subtracted
the same three from the FALSE subset of seventeen, applying the test count of the total population to
a subset — a count borrowed from one population and spent in another. Its figure is described, not
restated, for the reason given at the entry-statement population above. *Producer-loop exits* — 12 `continue;`. Command:
`sed -n '134,373p' apps/engine_producer_thread.cpp | grep -c 'continue;'` returns 12.

**Floor.** The `hostReady` write census is exact at 21 total (4 true / 17 false) with zero non-literal
arguments, and the ratchet also asserts no `exchange`/`compare_exchange` form exists — because a
`store` grep is blind to those. The producer-exit count is a floor of 12: a `return` or a `break` out
of a nested scope is not a `continue`.

**Failure model.** (1) Hung plugin with a writable socket — the case G3 exists for; the host renders
inline on the thread that reads its control socket, so SIGSTOP is a faithful constructor. (2) **The
only per-track detector is unreachable when the gate is shut**: `apps/engine_produce_block.cpp:1096-1101`
clears `hostReady` on `!sentOk`, but is not reached once production is blocked. (3) The producer's
three stall reasons are three distinct call sites — `logStall` with `minCompleted` (`:275`),
`inFlight` (`:285`) and `ahead` (`:328`) — and the claim that the local `minCompleted` is
OVERWRITTEN between them is FALSE and withdrawn. It is assigned once, at `:271`
(`minCompleted = progress.minCompleted`), and only read at `:275`, `:282`, `:285`, `:302`, `:306`
and `:328`. I asserted a data-flow hazard without reading the assignments, inside a failure model
whose entire subject is data flow.

**Deterministic test.** Three layers, no layer's verdict a duration. Layer 0 settles a contradiction
the packet must not carry into a verdict. Layer 1 decides the rule with no process and no clock. Layer
2 decides the rule is reached and the session survives. **The Layer-2 fixture SHALL set
`DAW_ENGINE_DEBUG_STALL`**: every stall line is gated on it (`apps/engine_producer_thread.cpp:62`,
`:80-82`), so without it the channel the PASS bullet reads does not exist — the omission the
predecessor's open list dropped.

**PASS conditions.**
1. Layer 0 decided the experiment, not the engine's lifespan. **THIS BULLET DEPENDS ON AN M1 THAT
   DOES NOT YET EXIST**, and an earlier version said the opposite on the strength of a static
   settlement R15 has since retracted: a real drop path
   (`apps/engine_produce_block.cpp:1096-1100`) means nothing static separates "gated and stayed
   gated" from "gated, dispatched, send failed, host dropped". This bullet is therefore NOT
   satisfiable until the telemetry M1 needs is built — which is what item 36 blocks on, and is
   PACKET work here as well as product work. *REFUTED BY* any Layer-0 verdict that reads a
   duration or an engine lifetime instead of the experiment's own outcome — and specifically NOT by
   `kill -0`, which R15 retracts as an instrument here: it answers identically for a stopped and a
   running process, so it never established the state this bullet is about.
2. The Layer-1 boundary is exact and the bound is COUNTED: not evicting at observation N−1, evicting
   at exactly N. *REFUTED BY* eviction at N−1, or non-eviction at N.
3. **The counter resets on advance, not on catching up**: a host that owes, advances one block per
   observation and stays `numBlocks−1` behind for 3N observations is never evicted. *REFUTED BY*
   eviction of that host.
4. **Absence is not non-advancement**: a host absent from the observation vector for 3N iterations —
   the try_lock drop at `:231-233`, which a ten-second Zebra2 load causes — is not evicted.
   *REFUTED BY* eviction on absence alone.
5. The mutation control requires the new boundary assertions to go RED while all 21 existing `CHECK`
   statements stay GREEN. *REFUTED BY* any existing CHECK turning red, which would mean the rule
   changed behaviour it was not meant to touch.
6. The session survives on a surface no current check touches: zero `render.stalled` events and
   `blocks` equal to the total. *REFUTED BY* any `render.stalled`.
7. The eviction lands on `hostReady`, not merely on the producer's vector. *REFUTED BY* the producer
   advancing while `hostReady` stays true.
8. Byte reproducibility with the fixture condition that makes it mean anything. *REFUTED BY* two runs
   differing under `cmp -s`.
9. The negative control exits 2 with the difference named in advance, restored by **cp-backup rather
   than `git checkout --`**, which would revert to HEAD and delete the uncommitted work. *REFUTED BY*
   a control that exits 0.

**Static checks.** Eviction is observed where the frozen gate still runs — an ORDER property invisible
from any return value, so the observation must be lexically inside the collection loop. No clock in
the rule: the eviction function's span contains no `chrono`/`steady_clock`/`system_clock`/`time(`
token. The containment event is emitted on a TRANSITION, appearing exactly once under `apps/` and
guarded by the eviction transition. The `hostReady` write-form ratchet, 21 total with 4/17 and no
`exchange` forms.

**Review register.** **The owner-ruling demand for N is DISCHARGED and this sentence used to
restate it.** R3 authors N = 3 and item 19 records that no further ruling is required; nothing in the
tree sources N, and all three production `Watchdog`s use `hardTimeoutBlocks = 500`, which is why an
authored constant was the only available answer. What remains for N is implementation and static
validation against the authored value, not a decision.
The reviewer SHALL rule on which of two contradictory statements is stale, and record the ruling
next to BOTH — `tools/host_stall_check.sh:16-19` and the producer comment. **R15 DOES NOT DISCHARGE THIS**, and an
earlier version of this sentence said it did. R15 defines M1 and rules on the register's framing, but
its central claim — that the mechanism was decidable at the pin and the check was the stale side — is
RETRACTED: a real drop path exists at `apps/engine_produce_block.cpp:1096-1100`. The demand stands,
and it now waits on telemetry that does not exist, per item 36. The reviewer
SHALL confirm that evicting a host which still owes dispatched blocks cannot corrupt what a RESUMED
host reads or publishes. **R16 DISCHARGES THIS ONLY FOR THE PRODUCER-VECTOR READING**, which PASS 7
forbids anyway: under `hostReady = false` the producer, the input/output write and the mixer all
skip the host, and SIGCONT cannot readmit the stopped process. It does NOT discharge the mixer's
snapshot path, where readiness is live and the mapping is not — item 37. The demand stands for
that half. The reviewer SHALL decide `daw::Watchdog`'s fate — deletion or
re-specification — since the gate accepts either with equal force and deliberately cannot choose.
The reviewer SHALL choose the resume mechanism R16 leaves open at item 37 (G3): a host-side
currency test, a reader-side monotonic clamp, a re-baseline on resume, or a per-slot block stamp.
**The reviewer SHALL resolve the static-check contradiction**: one check places the eviction inside
`apps/engine_producer_thread.cpp:225-252`, whose natural implementation was said to move the exit
count off 12, which a sibling check was said to forbid. **R17 DISCHARGES THIS AND ITEM 21 IS
CLOSED**: there is no sibling check — G3's four static checks never mention the exit count — and the
12 is a census row pinned to `PRODUCT_SHA`, compared for equality against a frozen tree the
implementation will not be on, and carrying an explicit floor besides. Neither statement has to
yield, so the demand that one of them must is withdrawn with this sentence.

---

# G4 — Segment and routed-audio ownership

**Severity** HIGH, not CRITICAL, and the reason is load-bearing: the default path is **already**
correctly gated by the mechanism this gate generalises. `apps/engine_audio_callback.h:328/:347` and
`apps/engine_master_render.cpp:80-95` both wait on `completedBlockId`, and the sidechain read at
`apps/engine_produce_block.cpp:907-919` indexes by the COMPLETED id. Those are counter-examples
proving omission rather than design, and they bound the blast radius to three shapes: chains
interleaving VST and non-VST devices, a patcher audio node after a plugin, and track-to-track routing
from a plugin-bearing track. **Dependencies** G0-A, G0-B, G1-A, G1-B, G2-A, G2-B, G3. Final gate — and therefore NOT DECIDABLE at
this SHA, on TEN dependency blockers plus one of its own.
**The ten**, carried by gates G4 depends on, are every blocker except its own 26:
18 (G2-B) the absent PASS 4 replacement, 19 (G3) no source for N,
24 (G0-B) the rename PASS 9 is RED without, 27 (G2-A) the class-wide missing sender identity,
28 (G2-A) the unauthored S3 extraction, 29 (G2-A) the malformed SetRowOps zero-base refusal,
33 (G1-B) the missing tests, 35 (G0-B) the EventEntry parity,
36 (G3) the non-discriminating stall check, and 37 (G3) the readiness/mapping generation gap.
Item 18's label used to read "the mirror-ack circularity"; R2 dissolved that and the label outlived
it. **An earlier version of this
sentence said FOUR and named only the first four**, a figure that stopped tracking the blocker list
as items were added and promoted. It is a count over that list, not a hand-kept subset: G4 depends
on every gate, so every blocker on another gate blocks it. The four named above remain the ones
whose SHAPE was analysed here; the rest block by the same rule without a separate argument — G2-A is one of G4's dependencies, so its missing sender identity blocks G4 exactly as a missing
population would. Item 11 was a fifth until it CLOSED; leaving a closed item in a blocker list is
how a gate acquires a permanent-looking obstacle that no longer exists.
**The one of its own** is 26, whose reason has MOVED: G4's population is complete across both planes, and what remains is the missing adjacent multi-plugin fixture and the test surface for it. This sentence said "whose selector is proven partial" for several SHAs after item 26 itself recorded the move.
**TWO CONDITIONS, stated once.** (a) Every dependency GATE PASSES — not that its items are ruled,
which they already are. (b) G4's OWN population exists. Both are required; (b) has been lost twice
in edits to (a), and this paragraph carried it twice at once after the second recovery, which is
what a patched-head-and-tail produces.
**Invariant.** For every dispatch, identified by the QUINTUPLE (host generation `g`, `blockId b`,
`segmentStart s0`, `segmentLength sl`, readiness level `r`) — the fifth component is R2 propagated:
a dispatch minted at `mapped-and-bypassed` and one minted at `mirror-complete` are DIFFERENT
dispatches even with the same four other fields, and an acknowledgement must name the level it was
minted under or a stale-level ack satisfies a fresh-level read — not by the loop ordinal, because segmentation is recomputed
from `trackState.chainDevices` every block (`apps/engine_produce_block.cpp:660-716`) so a chain edit
renumbers ordinals while `(s0,sl)` still names the same plugins — the bytes of that host's INPUT plane
slot for `blockIndex = b % numBlocks` are owned by the host from dispatch until that exact segment
acknowledges consumption. **The full ordering is `write_output → release-ack → acquire-wait → read_output`**: the host writes its output, releases the acknowledgement with release ordering, and no
consumer reads that output before an acquire-load of that acknowledgement. The predecessor stated
input immutability and ack identity but omitted this ordering, which is what makes the output half
sound rather than merely acknowledged.

**Population.** *Dispatch sites* — RAW 17 (`git grep -n sendProcessBlock`) → minus 14 (8 in test
mains, 6 non-calls: a design doc, a tools script, a log line, a declaration, a comment and the
definition) → **3 production**. ⟂ The predecessor stated the split beside the command instead of as a
subtraction, which put it outside the arithmetic the gate checks. *Out-plane readers* — RAW **27**
(`grep -rn -e audioOutOffset -e safeAudioOutPtr -e audioOutChannelPtr -e auxOutputPlaneOffset apps/`)
⟂ → **THIS SELECTOR IS RETIRED and its population is SUPERSEDED, not withdrawn.** It covered the
OUTPUT relation only while G4's invariant has two halves; the census block now carries both, seven
derived IN rows and two hand-classified OUT rows with every member pinned. **The gate remains
undecidable for a different reason, which open item 26 (G4) now states: the POPULATION covers both
halves and ONE FIXTURE IS MISSING — an adjacent multi-plugin segment — together with its oracle.
This sentence used to say the gate's PASS conditions, deterministic test and static checks cover one
plane; item 26 withdrew that as false.** The population is rebuilt below from the mapped base; what follows
records why the selector had to go, because a retired selector with no reason invites its own
return.** The four RAW terms are four SPELLINGS, and the host uses a fifth: `grep -rn audioAuxOutOffset apps/`
returns 5 sites and ZERO of them match any RAW term. The engine spells the aux plane
`auxOutputPlaneOffset` (a function); the host stores `state.audioAuxOutOffset` (a member). Same
plane — the host's own comment at `:498` says "the engine derives this same offset via
auxOutputPlaneOffset(header)".
**Among the five is a BYTE-PRODUCING WRITE the population cannot see:**
`juce_host_process_main.cpp:657-665` computes `state.auxOutputPtrs[ch]` into the plane and
`std::fill(..., 0.0f)` zeroes every aux slot every block. G4's invariant is a consumer reading bytes
ANOTHER AGENT WROTE — the other agent is the host, and this gate's population is blind to it
writing. That is the `S-1` spelling defect at the scale of a whole agent.
**R7 (below) rules the host IN SCOPE. THE REST OF THIS PARAGRAPH IS THE RECORD OF A REBUILD THAT
HAS SINCE HAPPENED** — it is written in the tense of the decision and marked here, at its head,
rather than only at its close, because a reader who stops in the middle of it otherwise leaves with
"the census must be rebuilt" as the live state. The census was rebuilt, and the OBVIOUS rebuild was
also wrong: keying on `ShmHeader::audioOutOffset` STILL misses the host, because
`juce_host_process_main.cpp:487-504` walks a running accumulator —
`header.audioOutOffset = offset; offset += alignUp(outBlockBytes, 64); state.audioAuxOutOffset = offset;` — so the host's aux offset is numerically a DESCENDANT of the field and SYNTACTICALLY A
SIBLING. A census keyed on the field NAME misses it for exactly the reason the four-spelling census
did. claude-worker-1 proposed that shape and withdrew it themselves before it was built on.
**The implementable version keys on the MAPPED BASE.** Plane bytes cannot be touched without
reinterpret-casting a segment base pointer and adding an offset — `state.shmBase`, `track.shmBase`,
`shmView->base` — and that artifact cannot be absent, because it is the only route to the memory.
The exact count depends on how tightly the cast pattern is written (a strict pattern gives 10
non-test sites, 7 of them in the host; a looser one catching header-relative casts gives more), and
**the distribution is the point: on this census the host is MOST of the population, so R7 is not a
marginal scope call.** **That census is now DONE — see the complete role census below** — and this paragraph stands as the
record of what the four-spelling selector could and could not see.
**The 27 partition itself is total and sums**, measured by claude-worker-1 and reproduced here:
11 non-code · 3 test mains · 2 header-field writes (`juce_host_process_main.cpp:496`,
`engine_ui_shm.cpp:40` — these assign the OFFSET FIELD, not plane bytes, a role my earlier partition
folded into "writes") · 4 establishing · 5 direct reads · 1 indirect read
(`engine_produce_block.cpp:1150`, dereferenced inside `enqueueInboundAudio`) · 1 host establishing.
11+3+2+4+5+1+1 = 27.
**THE SEVENTH READER IS RESOLVED AND IT IS OUTSIDE THE 27.** codex-worker-1 named it and it is
verified here: `EngineAudioCallback::process` (`apps/engine_audio_callback.h`) takes the
`planeByteOffset` derived at `engine_consumer.cpp:670`/`:730-731`, resolves a channel pointer at
`:397-404` (`reinterpret_cast<uint8_t*>(track.shmBase) + *planeOffset`) and CONSUMES samples at
`:448` (`trackChannel[i] * channelGain`) and `:463` (`ring[w] = trackChannel[i]`). It names none of
the four RAW terms, which is why the name-census could not see it — and
`engine_produce_block.cpp:1150` is the indirect ROUTING read already counted, not this one.
**Seven logical readers.** The seventh had to be NAMED by someone who knew where to look rather than
found by the selector, which is the whole argument for rebuilding the census on the mapped base.

**A NEGATIVE RESULT, recorded because re-chasing it would cost someone a day.** Two independent
derivations of one address is this project's most expensive shape, so claude-worker-1 checked
whether they agree: host `audioOutOffset + alignUp(numChannelsOut * stride * numBlocks, 64)` against
engine `audioOutOffset + alignUp((numChannelsOut * stride) * numBlocks, 64)` — the SAME VALUE, no
live divergence at this SHA. The duplication is a standing RISK (two expressions, one address, no
assertion tying them), recorded in the review register as a risk and NOT as a defect, because
nothing is currently wrong.

*Input-plane writers in engine production code* — RAW 13
(`grep -rn -e audioInOffset -e safeAudioInPtr -e audioInChannelPtr apps/`) → minus 11 (tests,
declarations and reads) → **2**. ⟂

**THE PLANE-OFFSET DATAFLOW, derived interprocedurally — this is what a name census cannot see.**
There is ONE origin. `juce_host_process_main.cpp:487-505` walks a running accumulator and both
planes fall out of it:

    offset = alignUp(sizeof(ShmHeader), 64)
    header.audioInOffset  = offset ;  offset += alignUp(inBlockBytes,  64)
    header.audioOutOffset = offset ;  offset += alignUp(outBlockBytes, 64)   <- MAIN, published
    state.audioAuxOutOffset = offset ; offset += alignUp(auxBlockBytes, 64)  <- AUX, host-private
    header.ringStdOffset  = offset

The aux offset is therefore **published nowhere**: the host keeps it in a private member and the
engine RE-DERIVES it — `auxOutputPlaneOffset(header) = header.audioOutOffset + alignUp(outBlockBytes * numBlocks, 64)` (`shared_memory.cpp:52-57`). Two expressions, one address.
**They agree at this SHA and the reason is a trap worth naming:** `outBlockBytes` DENOTES DIFFERENT
QUANTITIES in the two files — the host's includes `numBlocks` (`:491-493`), the engine's does not
(`shared_memory.cpp:54`) — and the values match only because each multiplies by `numBlocks` at a
different point. A future edit that simplifies either expression by trusting the identifier will
diverge them silently.

A third carrier crosses into the callback: `TrackInfo::planeByteOffset`, set from
`header->audioOutOffset` at `engine_consumer.cpp:670` for a normal track and from
`auxOutputPlaneOffset(*parent)` at `:730` for an aux child, then read as `planeBase` at
`engine_audio_callback.h:385`. **A reader reached through this field names no offset symbol at
all**, which is exactly why the four-term census could not see reader 7.

**THE SEVEN BYTE-CONSUMING READERS, with the route each takes to the bytes:**

    #  site                              plane  route
    1  engine_produce_block.cpp:923      main   sh->audioOutOffset + srcBlock*blockBytes, direct
                                                (sidechain, indexed by the COMPLETED id)
    2  engine_produce_block.cpp:1030     main   safeAudioOutPtr() -> memcpy SOURCE
    3  engine_produce_block.cpp:1112     main   safeAudioOutPtr() -> outputPtrs -> currentInput
                                                -> patcher node          (helper-mediated)
    4  engine_produce_block.cpp:1150     main   safeAudioOutPtr() -> routePtrs -> routeChannels
                                                -> enqueueInboundAudio   (deref in the CALLEE)
    5  engine_master_render.cpp:100      main   header->audioOutOffset + ..., direct (master mix)
    6  engine_consumer.cpp:766           aux    auxOutputPlaneOffset(*h) -> peak scan
    7  engine_audio_callback.h:404       either track.planeByteOffset -> track.shmBase + offset,
                                                consumed at :448 and :463 (NAMES NO OFFSET SYMBOL)

Four of the seven reach the plane through a helper, a struct field or a callee. **That is why every
name-based census of this population has been wrong**, and why the in-scope selection STAYED open at
item 26 until the mapped-base census replaced the four-term grep. **That replacement is DONE** — the
complete role census below is the mapped-base measurement, and item 26's remaining gap is the
adjacent multi-plugin fixture, not the selection.

**THE COMPLETE ROLE CENSUS, measured by claude-worker-1 from the MAPPED BASE and alias-followed —
23 role-bearing sites, and the arithmetic closes.** Role assigned by USE at the dereference, never
by variable name.

    7  cross-agent READERS      the seven above (engine consuming what the host/plugin wrote)
    7  host byte WRITERS        juce:664-665 fill · :686 memcpy into aux · :719 memcpy into out ·
                                :725-726 fill · :925 dst · :952, :956 fill
    1  host INDIRECT writer     juce:989/994 — outputPtrs and auxOutputPtrs handed to the plugin
    2  same-agent READBACK      juce:834, :1015 — the writer reading what it just wrote
    2  host ESTABLISHING        juce:638 main · :656 aux
    4  engine ESTABLISHING      engine_consumer.cpp:670, :730 · engine_produce_block.cpp:861, :866
    --
    23      7+7+1+2+2+4 = 23. The RAW-27 remainder is 16: 11 non-code, 3 test mains, 2 header-field
            assignments — of which 14 are not sites at all.

**AND THE SELECTOR'S BLINDNESS IS NOW QUANTIFIED, which is what makes it retirable rather than
merely suspect:** of the seven cross-agent readers the four RAW terms match SIX; of the eight host
writers they match ZERO of the aux ones, because those reach the plane through `audioAuxOutOffset`,
which is not in the selector. So the four-spelling census understates the read side by one and the
write side by most of it, and cannot be a floor for either.

**THE CENSUS BLOCK — every role count stated ONCE, as a command that returns it.** Backend changed
R8's `7/8` to `70/80` and R9's `one` to `two` in the packet text and the gate still passed, because
those numbers lived in PROSE and nothing re-derived them. Each row below is executed by A.0 against
the pinned tree, so the same mutation now fails as `COMMAND-MISMATCH`. Rulings and items CITE a row
by role name; **no ruling restates a count**, which is also the fix for the drift that had R5
excluding writes while R8 included them.

    IN plane — the ownership half, and the half open item 26 (G4) found missing. Every row DERIVED,
    and every row's MEMBERS pinned in A.0's roster, not only its count.
      engine addressing sites            `git grep -n -E 'safeAudioInPtr\(blockIndex|header->audioInOffset \+' apps/engine_produce_block.cpp apps/engine_master_render.cpp | wc -l` returns 2.
      engine byte-producing writes       `git grep -n -E 'std::(memcpy|fill)\(input' apps/engine_produce_block.cpp | wc -l` returns 11.
      master summed-mix write            `git grep -n -E 'const_cast<daw::ShmHeader\*>\(header\)\) \+ off' apps/engine_master_render.cpp | wc -l` returns 1.
      host plane-address acquisitions    `git grep -n -E 'audioInChannelPtr\(state.shmBase|const float\* const\* (plugin)?[iI]nputPtrs|= pluginInputPtrs\[ch\]' apps/juce_host_process_main.cpp | wc -l` returns 6.
      host byte loads                    `git grep -n -E 'state\.inputPtrs\[(src|ch)\],|std::copy\(src,|dst\[i\] = src\[i\]|std::fabs\(src\[i\]\)|std::fabs\(static_cast<double>\(src\[i\]\)\)' apps/juce_host_process_main.cpp | wc -l` returns 6.
      host indirect handoff              `git grep -n 'process(pluginInputPtrs' apps/juce_host_process_main.cpp | wc -l` returns 1.
      alias leaves the plane             `git grep -n 'inputPtrs = outputPtrs;' apps/juce_host_process_main.cpp | wc -l` returns 1.

**THE UNIT WAS MIXED AND codex-worker-1 NAMED IT.** One row previously counted five and meant
"acquisitions and direct memcpy sources together" — address sites and byte loads in one figure. They
are now separate rows with separate units: SIX acquisitions (`:644` establishes, `:862` and `:879`
are the two alias hops, `:924`/`:939`/`:975` take a channel address) and SIX byte loads (`:686` and
`:720` read through `state.inputPtrs` directly; `:927`/`:930` are the level-matched bypass copy,
`:942` the bypass meter, `:980` the input meter). A census whose unit shifts inside one number
cannot be checked by anyone, including its author.

**AND FOUR OF THOSE LOADS ARE PLANE READS ONLY FOR THE FIRST PLUGIN IN A SEGMENT.** `:862` binds the
alias to the plane BEFORE the per-plugin loop at `:866`, and `:1061` rebinds it —
`inputPtrs = outputPtrs;` — at the end of every iteration. So `:927`, `:930`, `:942`, `:980` and the
handoff at `:987` touch the input plane on the first plugin and host-local output buffers on every
plugin after it. **That is a positional rule, and no site list can carry it**, which is why
`alias leaves the plane` is a row: the fact that the population ENDS is a member of the population.
**It does NOT narrow G4's ownership window**, and an earlier version of this paragraph said it did.
The invariant runs from dispatch to the segment's acknowledgement; there is no per-plugin signal for
the engine to observe, so what the host happens to read after plugin one cannot shorten a window the
engine has no way to learn about. What the rule does establish is a FIXTURE requirement: a
single-plugin segment exercises none of this, so the deterministic test must dispatch a multi-plugin
segment or it tests the one arrangement where the rebinding never happens.

    OUT plane — the ordering half (`write_output → release-ack → acquire-wait → read_output`).
    BOTH ROWS ARE THE HAND-CLASSIFIED ROLE CENSUS OF THE POPULATION BULLET ABOVE — they carry no
    marker of their own because they are not a second population, and a marker here would inflate
    the hand-classified count by restating one. No single pattern reproduces either row.
      cross-agent byte-consuming reads   claims 7 — floor, name-reachable addressing only — `git grep -n -E 'safeAudioOutPtr\(blockIndex|audioOutChannelPtr\(' apps/engine_produce_block.cpp apps/juce_host_process_main.cpp apps/engine_master_render.cpp | wc -l` returns 4.
      host byte-producing writes         claims 8 — SUPERSET, includes addressing, zero-fills and the `:834` self-read — `git grep -n -E 'outputPtrs\[ch\]|auxOutputPtrs\[ch\]' apps/juce_host_process_main.cpp | wc -l` returns 13.

**THE TWO HAND-CLASSIFIED ROWS ARE PINNED BY MEMBER, not by selector.** No command derives the
seven readers or the eight writers — that is what makes them hand-classified — but each member is
CITED in this document, and a citation can be pinned even where a selector cannot. A.0 holds both
lists as (path, line, fingerprint of that line) and checks them against the product, and requires
the reader table above to cite exactly the seven. So a citation that goes stale against the pin
FAILS rather than ageing quietly into decoration, and the table and the roster cannot drift apart
the way the prose and the census did. What is still not checkable is whether the seven are the RIGHT
seven; that is the judgement the HAND-CLASSIFIED marker declares, and no amount of pinning converts
a judgement into a derivation.

**EVERY ROW IS BOUND TWICE, and neither binding lives in this file.** A.0 holds a roster of
(relation, role) → (member line numbers, claimed count) and requires the parsed rows to equal it
exactly: no row missing, none extra, none duplicated, no claim altered, and **each row's command
must return the roster's LINE NUMBERS** — so an arbitrary command that happens to return the right
count fails on its members. codex-worker-1 mutated this block five ways — deleting the master row,
relabelling IN as OUT, duplicating a row, substituting a command, altering a member citation — and
all five passed while A.0 checked only the counts. A roster written into this document would have
been one more statement in the file being mutated; it is in the script, which this packet blob-pins.

**Floor.** Dispatch sites floor 3: `rg` finds every syntactic call but is blind to a dispatch through
a function pointer. The WRITER census is a floor of 2 for the same reason plus helper indirection.
The IN-plane rows are floors for a third reason on top of both: an alias hop is a syntactic form, so
a THIRD hop — or a plane pointer stored in a struct field, which no row above would match — extends
every count. No reader floor is
QUOTED here, which is not the same as there being none: the OUT reader row two paragraphs above is
itself a floor and returns four, with seven authored members pinned beside it. The reason no floor
is quoted is that no selector establishes COMPLETENESS — the defensible claim — rather than the one
this sentence used to give, that the population was withdrawn. It was withdrawn when this was
written (open item 26 (G4)), and quoting a floor of 7 for a withdrawn population would have restated
the very number the withdrawal
removed. **Item 26 has since recorded that the population is complete and covers both planes**, so
the withdrawal this sentence relies on is history; the floor stays unquoted because the reason above
still holds, not because the population is still withdrawn.

**Failure model.** (1) Input overwritten under the host: the host is delayed before reading segment
k's input; the engine memcpys segment k+1 over it (`:944-1037`). (2) Stale lap read as fresh: the host
is delayed before WRITING segment k's output, and `:1030-1033` reads the audioOut slot for the same
`blockIndex`, which still holds the block from `numBlocks` ago — ~32 ms at 512@48k. (3) Torn input
plane, with no generation or marker to detect it. (4) Routing copies the same un-awaited plane
(`:1147-1154`). The host publishes `completedBlockId` only on the last segment
(`apps/juce_host_process_main.cpp:1069-1087`), so **there is no per-segment signal to wait on even in
principle** — which is why the invariant demands an ack keyed on the quintuple.

**Deterministic test.** Stated first: the device-free offline render (`apps/daw_engine_main.cpp:2143-2153`)
is the byte-exact oracle for CONTENT and is the only driver this gate accepts for content, but its only
barrier is per-block, so ORDERING is decided by a barrier-controlled fake host emitting a merged log.
**Chains, defined here because the predecessor referenced them and compressed away their
definitions:** F0 = one track, single VST, unsegmented (the determinism baseline). F1 = VST → patcher
audio node → VST on one track, which segments into two dispatches on one blockId. F2 = F1 with the
patcher node replaced by a non-VST device that produces no audio, isolating the segmentation boundary
from the audio path. F3 = VST → patcher → VST → patcher, three boundaries. F4 = F1 with track routing
to a second track. F5 = two tracks where B sidechains from A, exercising the carve-out. F6 = F1 with
the master bus engaged, which must remain unsegmented. **Schedules:** S1 no holds (the control); S2
hold the host before reading input; S3 hold before writing output; S4 hold after writing output and
before the ack; S5 hold the engine between dispatch and the next segment's input write; S6 reorder two
segments' replies; S7 kill the host mid-chain; S8 the multiplier sweep, every other schedule re-run
with holds ×1, ×10, ×100. **The
negative control is specified against "the pinned tree PLUS this gate's instrumentation, with the
per-segment ack and wait absent"** — an unmodified pinned build cannot emit the merged log at all, so
the predecessor's control could not run.

**PASS conditions.**
1. Ack census, non-vacuity first: exactly one ack per `recv_request`, and the count equals what the
   fixture's chain implies — **F1/F2: 2, F3: 2, F4: 2 on track A**, and for F6 **exactly ONE
   `recv_request` per `masterBlockId`**, which is the master-stays-unsegmented ratchet. *REFUTED BY*
   any count differing.
2. Ordering in the merged log: `recv_request` before `read_input` before `ack`; and every
   next-segment `write_input`, patcher `read_output` and routing read strictly after that ack.
   *REFUTED BY* any pair inverted under any schedule.
3. **Output ordering**: every `read_output` is preceded by an acquire-load of the acknowledgement
   whose release-store follows the host's `write_output`. *REFUTED BY* a `read_output` with no
   intervening acquire, or an ack released before the output write.
4. Input immutability observed in the BYTES, not the schedule: every AFTER_INPUT_READ snapshot equals
   what the engine's `write_input` recorded for that quintuple. *REFUTED BY* one differing byte.
5. Full-identity comparison, not a field echo: each consumer refuses when ANY ONE of the FIVE
   components is wrong and the other four right — including the readiness level, which R2 added.
   *REFUTED BY* a consumer reading on four of five.
6. Generation is engine-minted and host-echoed. *REFUTED BY* a generation the host originates.
7. Segmentation-isolating content: three renders of F0 byte-identical, three of F1 byte-identical.
   *REFUTED BY* any pair differing.
8. Stale-lap priming and absence: the previous lap's content is non-zero, distinct, and differs from
   the value it could masquerade as. *REFUTED BY* a zero-filled slot, which would make the assertion
   pass for the wrong reason.
9. **Wall clock out of the decision**: re-running every schedule with holds ×1, ×10, ×100 produces the
   identical verdict and byte-identical WAVs. *REFUTED BY* any verdict or WAV that changes with the
   multiplier.
10. Sidechain carve-out preserved: with track A pinned at b−2, track B's key channels carry A's b−2
    sentinel. *REFUTED BY* the key carrying b's value.
11. **Output equality in the BYTES — the mirror of 4, and the bullet whose absence let a stale-slot
    read satisfy this whole gate.** Every consumer's BEFORE_OUTPUT_READ snapshot equals what the
    host's `write_output` recorded for the SAME quintuple, and differs from the lap primed by 8.
    Conditions 3, 5 and 8 are each satisfied by a read of stale content: 3 decides ORDERING (an
    acquire happened), 5 decides IDENTITY (the four fields are right), and 8 decides only that the
    priming is distinguishable — it establishes the precondition for catching failure model (2) and
    never asserts the failure is absent. Failure model (2) is a byte-level claim and needs a
    byte-level bullet. *REFUTED BY* one differing byte, by a snapshot equal to the primed lap, and —
    the vacuity guard — by a fixture in which the fresh and primed payloads compare equal, which
    would make this pass for the reason 8 exists to prevent.

**Static checks.** Ack-after-process in the host: the per-segment acknowledgement is stored AFTER the
last `slot.instance->process(...)` (`apps/juce_host_process_main.cpp:987`). Both dispatch branches:
the wait is placed after the join of `if (debugStall) … else …` (`apps/engine_produce_block.cpp:1073-1095`),
not inside either arm. Refuse-on-timeout shape: any deadline at `:1030`, `:1112` or `:1150` places the
READ inside the success branch. The compared value must have a host-side producer — it must be the
word the HOST stores, never `sentOk`.

**Review register.** The reviewer SHALL read `juce_host_process_main.cpp:925` beside `:1015` before
accepting any role classification in this gate: `float* dst` at :925 is a DESTINATION and
`const float* dst` at :1015 is a SOURCE — the same identifier, opposite roles, ninety lines apart in
one file, separated only by a `const` and what the loop does with it. That is the defect that
produced this packet's out-plane retraction (`float* output` used as a memcpy source) living in the
same file in a second pair, and it is why "classify by USE, not by name" is a rule here rather than
a correction. The reviewer SHALL rule on the DUPLICATED AUX-OFFSET DERIVATION: host and
engine compute the same address by two independent expressions with no assertion tying them. They
agree at this SHA — verified, not assumed — so this is a risk to pin, not a bug to fix, and pinning
it costs less than the day it takes when they diverge. The reviewer SHALL confirm by reading
`apps/juce_host_process_main.cpp:606-1096` that the SHIPPING host stores the acknowledgement after
the last `process`. The reviewer SHALL confirm
G3 has passed and that the adopted wait carries an independent per-host deadline whose expiry FAILS
the render with a diagnostic rather than proceeding. The reviewer SHALL confirm the new wait cannot
re-enter either deadlock door documented at `apps/engine_rt_helpers.h:200-234`. The reviewer SHALL
confirm where the acknowledgement lands and accept the consequences: inside `BlockMailbox`'s
`reserved[11]` (`apps/shared_memory.h:499`) nothing moves; growing the mailbox is a `kShmVersion` bump.

---

# Implementation constraints (non-negotiable, backend 2026-08-10)

1. Production atomic **size/alignment/`is_always_lock_free` assertions** and the canonical checked
   `LayoutSpec` land FIRST, before any gate's fix.
2. **One coordinated `kShmVersion` bump** for layout/snapshot/ring changes; `kControlVersion` if the
   host ring or Hello changes; `kPatcherAbiVersion` separately, carrying
   `{magic, version, struct_size, status}`.
3. `SHM_LAYOUT.md` and the C++↔Rust generated parity are updated in the **same changeset**.
4. Barrier fixtures run on **macOS arm64 AND Windows x64**. A timing or stress result is not accepted
   in place of a deterministic transition test. This project has a documented history of "zero
   underruns, therefore correct" conclusions that were wrong, so the rule is stated, not assumed.

# A.0 — the gate this packet is decided by

`tools/p12_selfcheck.py` at this SHA, PREV PACKET BLOB `9364bad5918360b1334af06620b3dc1639046c21`, A.0 SCRIPT BLOB `9a6cb1cd56230888a6b04aebf134848e04b23f0b`
— the script hashes itself and refuses if the packet pins a different blob, so the gate and the
document it decides cannot move apart. It refuses to run unbound: `AE_P12_PIN` must name a checkout of
product `75c6f064` whose tree is `699abfe8` with zero modified paths, and the packet file must equal
its committed blob unless `AE_P12_DRAFT=1` is set for an unpublishable draft run. Every refusal exits
**2**, so a broken gate can never be read as a passing one. Invocation and expected output:

    AE_P12_PIN=<pin> python3 tools/p12_selfcheck.py
    packet blob <oid> · product 75c6f064 tree 699abfe8 · 39 items, 30 open · 14 RAW (13 hand-ruled) + 32 commanded-claim occurrences, all executed
    PASS

**The MANIFEST is the canonical machine-readable source, at `ae-p1.2-manifest/7`.** **A SEMANTIC
WIDENING IS A VERSION CHANGE EVEN WHEN THE JSON SHAPE IS UNTOUCHED**, and /5 broke that: the
observable invariant moved from `kind` marks EQUALLING the blocker set to blockers being a SUBSET
of the marked items, and a consumer that had read the equality as schema semantics got no warning
because no key changed. codex-worker-1 raised it as a compatibility caveat. /6 carried that
widening as well as the new `unclassified_open` population — **and /6 then went out TWICE with
different gate shapes**, once with `plannable_with_dependencies` and once without it and with
`planning_verdict` beside it. That happened in the commit AFTER the one that wrote the rule, and the
rule had been applied correctly in the same edit to the widening it was written for. **Writing a rule
is not obeying it, and the gap between the two was one commit** — a rule feels like context rather
than a constraint at exactly the moment you are the one changing something. codex-worker-1 compared
the key sets. `/7` is the version the second shape should have had.
`docs/architecture/tasks/AE-P1.2-manifest.json` carries the gates and their decidability, the
rulings with `named_at` — every NON-HEADING mention of each one, with line and context, the
heading itself being carried by the record's own `line` field; there is no `applied` field, because
a ruling is applied TO ITEMS and its items can disagree — every item with its gate and blocking/closed state, every RAW
claim with its command and arithmetic, the control names, the UNCLASSIFIED OPEN population, the
three-valued `planning_verdict` with the gates each unknown comes from, and the counts. It is EMITTED by this
checker from the same extraction the checks run on (`--emit-manifest`) and the checker FAILS if the
committed copy differs, so it is canonical without being a second hand-maintained document — which
is the defect this packet has produced in every other form today. Read the manifest for facts and
the prose for reasons.

**The two claim categories are DISJOINT**, which they were not until this SHA: a RAW claim whose
command states a return was counted as a RAW claim AND as a commanded claim, so the printed sum
exceeded the population it summed and neither number meant what it said. A command belonging to a
RAW claim now belongs to it alone. The correction removed exactly one claim from the commanded
count, which is the size of the overlap and not an estimate of it.

**What it decides.** Open-item header against body, contiguity, and orphaned numbers. Every
`open item N (Gx)` cross-reference resolving AND naming the gate that item belongs to — three
references in the predecessor pointed at real items belonging to other gates, and every one of them
"resolved", so resolution alone was never the property at risk. Every PASS bullet carrying a
`REFUTED BY` or an explicit withdrawal. No withdrawn bullet described elsewhere as covered. Every RAW
claim carrying a runnable command that reproduces its figure, and every `RAW n → minus k → m`
satisfying `n − k == m`. Every runnable command in the document — not only the RAW-form ones —
stating what it returns and returning it; the predecessor's instrument decided one shape and left a
dozen exact claims written in the other shape unexecuted. No command written in `rg`.

**What it does NOT decide, stated because a gate silent about its limits reads as total coverage.**
Whether a RULE's subtraction is *justified* — it checks the arithmetic, not that the excluded lines
deserved excluding. **Membership and dataflow.** Every check here counts, matches text, or verifies arithmetic. None
of them decides whether a named site actually READS the bytes it is classified as reading, whether
two spellings denote one address, or whether a pointer reaches a dereference through a helper. Every
population defect found in this packet — the four-spelling census that missed the host, the seventh
reader that had to be named, `:1030` classified a write because a variable is called `output` —
was invisible to A.0 by construction and was found by a person reading code. A gate that counts
cannot see membership. Whether a population's predicate is the right one: three figures in this packet
were re-derived against a predicate of the author's that was wrong (mentions for call sites, `apps/`
for the whole root, a forwarding lambda for an emit site), and in all three the packet was right and
the recheck was wrong. Whether this commit is the intended one. The binding pins the PARENT commit and the parent's
packet blob, so the chain is immutable link by link — but two sibling children of the same parent
are indistinguishable from inside either of them, because no commit can contain its own hash. The
tip SHA and packet blob in the announcement are the external pin; a reviewer must take them from
there, not from the document. Whether the rename R4 chose for open item 24 (G0-B) is the right
resolution — the gate can see the two names differ; it cannot judge a design choice, and R4 made
one. (This sentence previously said the choice was unmade, which R4 falsified and I did not come
back to: A.0's "what it does not decide" list is the exact place where a stale sentence understates
what has been settled.)
And nothing about the product beyond what a text search can see.

**Emission is the LAST thing A.0 does.** The manifest write sat above a dozen later checks with an
`if fail` guard in front of it, so every late failure still published — codex-worker-1 reproduced it
twice, once renaming a review-register heading to emit a structurally incomplete manifest with exit
code 0. A guard placed early tests only the failures known early. The write is now after every
check, staged and atomically replaced, and `MANIFEST-STALE` is suppressed on an emitting run because
a check that forbids its own remedy is a deadlock rather than a guard — that deadlock appeared the
moment the write moved, and is recorded here because it is the predictable cost of the fix.

**Executable proofs.** Six, each a flag on the same script, because a check verified at somebody's
shell prompt is evidence for that person and nobody else. `--prove-blanking` is a DIFFERENTIAL, not
an absolute: text injected outside a hiding place must raise the count and text injected inside must
not, so the blanking is shown to be doing work rather than merely agreeing with its author on a clean
document. `--prove-emit-identity` drives a real failure into a real run and requires the committed
manifest to be byte-identical afterwards — it REFUSES when the artifact is absent, because
`None == None` proved nothing the first time. **AND THE LEDGER'S FIRST VERSION WAS SHALLOW IN FOUR MORE WAYS**, all found the same day it
shipped: it recorded no leaf TYPES, so `items[].kind` going from an array to a comma-separated
string passed with the key set intact; it merged records first-wins, so a field present only on the
SECOND ruling never appeared; "append-only" was a sentence, so rewriting an entry beside a new field
passed; and the single declaration could be RELOCATED out of A.0 and still parse, because uniqueness
without scope is not a place. The shape is now names, container kinds and leaf types over ALL
records at every depth; the ledger is compared against the predecessor's under an EPOCH, so
recomputing every hash is possible and visible rather than possible and silent; and the declaration
is read from A.0's section. **The element type of a scalar list is the UNION across
occurrences, and an EMPTY list is no evidence** — recording it per occurrence made an empty list a
different shape from a populated one, so a gate gaining a reason string demanded a version bump, and
a guard that cries at values trains its reader to bump for nothing; dropping element types entirely
was the over-correction, since `string[]` becoming `number[]` then needed no bump either, which
backend named as a release blocker. A record list with SCALAR SIBLINGS is its own shape: filtering to
dicts made a bare string appended to `rulings` invisible, and a consumer iterating records would get
it.

**AND THE PROOF WAS STILL NOT BOUND TO THE GUARD.** With the predicate in one function and the
ACTION in a separate `if`, sabotaging the `if` left all twelve cases green — a proof of a predicate
is not a proof of a guard. There is one call site that computes AND reports, it records that it ran,
and the proof refuses before its own cases unless the production invocation already happened.
Deleting that line now makes the proof say so, which I ran.

**THE SCHEMA GUARD IS A LEDGER, not a comparison with a neighbour.** Its first version compared the
emitted key sets against the manifest at `PREV_TIP`, and codex-worker-1 walked through it four ways
in one pass: a version ROLLBACK passed because the parent had that shape under the old version, a
key added to ONE record was invisible because the walk read element 0 and called it the list's
shape, nested lists like `rulings[].named_at[]` were never reached at all while the comment claimed
"every level a consumer binds to", and the proof called the CLASSIFIER rather than the branch, so
sabotaging the production refusal left all six cases green. Every published version now records the
shape it went out with, entries are append-only, and the current shape must equal the entry for the
current version — a rollback fails because the retired version's recorded shape is not this one, a
shape change under a held version fails because the hash moves, and neither depends on which SHA
happens to be the parent. The walk takes the UNION of keys across every record and reports a
heterogeneous list as its own defect, because a consumer binding to a list of records binds to one
shape and a heterogeneous list has none.

**`--prove-schema-guard` exists because a check no
CONTROL can reach needs one**: no packet mutation can change the manifest's SHAPE, since the shape
is fixed by the emitter, so the control written for `SCHEMA-UNVERSIONED` reported BLIND and was
right to. The proof synthesises a predecessor and runs BOTH arms — four shapes that must be refused
under a held version, and two that must not, one of them a version that WAS bumped and one a change
of values with the shape intact. It is bound to production like the extractor ratchet: the live
comparison is the same function, and a proof exercising a private copy says nothing about the run.
**This is the second check in one day that I nearly shipped with nothing able to trigger it**, and
both times the signal was a control reporting BLIND on its first run — which is the control doing
the one job a green suite cannot. `--prove-extractor-ratchet` holds the raw-prose
extractor count at its floor. `--prove-sweep-predicate` classifies a crafted-line corpus covering
every spoof a reviewer has landed on the sweep's success test — a bare `OK (+` prefix, an unanchored
tail, a wrong tag, a count the emitter cannot print, and a tag line with no separator.

**A RULING RULES; IT DOES NOT CARRY AN ITEM'S STATE.** R7's `Cost` clause said its census method
"is why item 26 stays open" — a reason superseded twice over, because the census is DONE and item
26's live blocker is a missing fixture. codex-worker-1 found that one. The same pass found a second
it did not name: R1 asserted "Item 11 and item 25 stay open FOR THIS WORK" while item 11 has been
CLOSED since R1 was authored. Two sentences, one shape, both wrong in the same direction — **a
restatement decays the moment the item moves, and every ruling is an opportunity to make one.** So
patching the two would have left the class open; the state is now read at the item and asserted
nowhere else, and three further rulings (R2, R3, R17) were reduced from asserting a state to citing
the item that holds it. **AND THE CITATION FORM WAS THE ONE PLACE A STATE COULD BE ASSERTED FALSELY.** `open item N (Gx)`
was EXEMPTED from the consistency scan as "a citation, not a predicate" — so the packet's own
reference form became unexaminable, and six live citations called CLOSED items open, four of them
item 11. codex-worker-1 found it inside the exemption I had pinned from both sides one commit
earlier, which is where an exemption is least examined. The word `open` in that form is a CLAIM and
is checked against the item.

**AND THE SCAN IS NARROWED, in two places, each a stated gap rather than a silent one.** Past tense
is history — `was open` made every historical sentence an assertion about now, and this packet is
full of them by design — and a NEGATED clause is not judged at all, because `does not remain open`
was refused for saying the opposite of what it says. If a past-tense or negated claim decays,
nothing here will notice. Both were codex-worker-1's, the second a false positive I created while
repairing the first. `RULING-ITEM-STATE` refuses the shape, per SENTENCE — split on its
terminator, not a character count — with one subtraction: **the packet's cross-reference FORM
`open item N (Gx)` is a citation, not a predicate.** That subtraction is load-bearing and pinned
from both sides: removing it makes R12's untouched citation refuse the pristine document.

**NOTHING HERE READ AN ITEM AS MARKDOWN, so an edit could leave a bold run open and every check
passed.** Editing item 38 at the predecessor SHA I replaced a sentence's opening clause and left its
closing `**` behind: the body carried an odd number of delimiters, a lowercase orphan fragment, and
a run a renderer closes wherever it next can — and the headline was intact, the counts summed, and
the manifest published the broken text verbatim as `items[38].body`. codex-worker-1 found it by
reading. Every check in this gate counted, matched or arithmetically verified; none of them looked
at the syntax the document is written in. **The check is a DELIMITER-PARITY one and claims no more
than that**: `**` runs are counted in the VISIBLE view, because a delimiter inside a code span is
literal text rather than emphasis, and an odd count per item is refused. It cannot see a mis-NESTED
pair, only an unmatched one — which is the defect an edit actually produces, and is the one that
occurred. Run against the predecessor's packet it names item 38 and nothing else.

**ESCAPES ARE ONE FACT WITH TWO CONSUMERS, and having only the first was the next defect.** The
parity check subtracted backslash escapes and the STATUS PARSER did not, so a status written
`**NOT BLOCKING\_EXTRA ...**` read as clean: the phrase's boundary lookahead saw a backslash, which is not a token
character, and the token scan split at it and found an exact `BLOCKING` — while a reader sees
`NOT BLOCKING_EXTRA`, the exact form the grammar claims to reject. codex-worker-1's probe. The two
consumers need OPPOSITE things from one fact, which is why they share a HELPER and not a transform:
the status parser needs the escaped character PRESENT because it is part of a word, and parity needs
it ABSENT from the delimiter population because it is literal text. Both are answered by knowing
which characters are escaped, derived once. **A BACKSLASH
ESCAPE IS NOT A DELIMITER**: `\**` is one literal asterisk followed by one emphasis character, and
counting `**` substrings saw a pair — so a real unmatched opener could ride in beside an escaped one
and the parity stay even. Escapes are inside this packet's Markdown subset, so an ALLOWED form was
fail-open; the whole CommonMark punctuation class is removed before counting, not the one character
the probe used. **And every per-item check is worth what the SECTION BOUNDARY is worth.** A `# `
heading inserted before the last item terminates every item span and the section with it, leaving
live text no item covers — invisible to parity, to the status grammar, to the manifest, and to
MANIFEST-STALE, which preserved the omission faithfully. **My first repair for that was a check
nothing could trigger**: a totality test over the section's interior, where the span regex already
covers every byte between one item and the next by construction. Its control reported BLIND, which
is the only reason it was written. A check nothing can trigger is worse than no check, because it
reads as coverage. The rule is at the boundary instead: a top-level heading is a GATE or one of the
document's named structural sections, and there is no third kind — a rule rather than a pinned
sequence, so it needs no edit when a gate is added and an invented heading has nowhere to be. **AND A HEADING MAY BE INDENTED UP TO THREE SPACES**, which `^#` saw none of — so a one-space
duplicate of an allowed name truncated the section above it and neither the membership check nor the
uniqueness check was ever shown it. The same allowance the open-items section slice already makes,
missed in the check written beside it. **AND MEMBERSHIP
IS NOT UNIQUENESS**: exact-matching the names closed the prefix hole and left a DUPLICATE of an
allowed name passing, which truncates the section above it while every word in it is permitted —
one property along from the one just repaired, found the same day. A section appears once because
it IS a section.

**A LITERAL CHARACTER SEPARATES ITS NEIGHBOURS, and deleting it FABRICATED a delimiter.** `*\_*` is
one star, a literal underscore and one star — no emphasis pair anywhere — and removing the
underscore butted the stars together into a `**` the document does not contain, which then cancelled
a REAL unmatched opener elsewhere in the item and left the parity even. Literal characters are
blanked now, not dropped. **And an HTML ENTITY renders to a character too**: `&#95;` produced the
underscore in `**NOT BLOCKING&#95;EXTRA.**` that the status grammar claims to reject, one notation
along from the backslash escape it was written for. Entities resolve through the same helper and
their characters are literal for the same reason.

**AND THE STATUS SPLITTER THREW THE FLAGS AWAY.** `_render` already knew which characters are
literal, and the run splitter read only its rendered text — so `\*\*BLOCKING...\*\*`, two LITERAL
stars and not an emphasis run, parsed as a declaration and emitted `blocking: true`; `&#42;&#42;`
did the same. **That is the parity rule applied in one of its two places**, in a helper written an
hour earlier so that both consumers could ask one question, and it is the shape this checker
produces more than any other. Delimiters are found in a MASK where literal characters are blanked,
and the run text is taken from the rendering at those offsets.

**AND MASKING THEM MADE THE REGION EMPTY RATHER THAN MALFORMED**, so the repair produced no status,
no tokens and no refusal — absence read as a value for the FOURTH time in this parser, arriving
through the fix for the third. A headline that reaches for the status word with no bold run to
declare it in is named for that. A LATER run that reaches and misses is named too: the earlier check
demanded an exact phrase, so `**NOT BLOCKING_EXTRA...**` parked on a continuation line was a status
attempt no rule was watching.

**AND THE TWO CHECKS HAD A SEAM.** Composing the probes — ordinary prose on line 1, literal-star
status on line 2 — walked between them: the bare-reach check read only the FIRST LINE and the run
scanner correctly saw no real delimiters. **Neither check was wrong; the seam was**, which is the
failure mode two correct checks produce and a single check cannot. The bare-reach arm reads the
whole item, and it only runs when there is no leading bold sequence at all, so prose deep in an item
that HAS one is untouched.

**AND HEADING UNIQUENESS COMPARED FULL STRINGS**, so a second `# Open items` with a different
derived tail was not a duplicate, and neither was a second `# G0-A — <another caption>`. Both are
duplicates of a SECTION and neither is a duplicate STRING. Uniqueness is by KIND now — the gate it
names, or the section it is — because a heading's kind is what a reader and a slicer bind to and its
tail is a caption.
**AND IT MATCHED BY PREFIX, so the final section's heading with one extra word appended passed as
the section it truncates**, carrying orphan text with an unclosed emphasis run that nothing ranged
over and no MANIFEST-STALE to show for it. **The heading is not quoted here, and that is a rule
rather than a style**: four controls anchor on that heading's opening words, and writing it into
this paragraph put their FIRST occurrence in the explanation instead of the document — all four
mutated prose, landed cleanly, and reported themselves BLIND. A control's anchor is a string the
whole document shares, so the text describing a control must not contain it.

**AND THE HARNESS COULD NOT SEE THAT, so `CONTROL-ANCHOR-AMBIGUOUS` now can.** It checked anchors
for the ZERO case only. An anchor is replaced by occurrence — `replace(old, new, n)` takes the first
n — so an anchor that occurs more times than its control declares is a POSITION-BASED selection with
no stated meaning, and it reads as green either way. Turning the check on found **fifteen** already
in the file: every one of them targeted a census row or a population datum whose prose mention came
LATER in the document, so all fifteen were correct BY ORDERING and none by construction. That is the
property my own paragraph broke by inserting an occurrence above four of them. Each anchor now
carries enough of its own line to be unique. `LAST` is exempt because it names the occurrence it
means; multiplicity is not ambiguity when the control says which one. The eighth prefix-where-an-equality-belonged in this checker,
and the shortest-lived: it was written the same day. Two of the headings carry derived tails and are
matched by their FULL patterns; the rest are exact.

**A STATUS PARKED AFTER PROSE was an absence the region could not see**, for the third time in this
parser and in a third disguise. `_status_runs` stops at the first thing that is not whitespace or a
marker, so `— some prose **NOT BLOCKING...**` yields an EMPTY region: no tokens, no status, and the
default answering "not blocking". The headline's first line is scanned outside the region as well,
and a status word found there that the region did not account for is refused.

**AND A GRAMMAR IS NOT A TOKENISER.** The parse below closed the three scan shapes and codex-worker-1
broke IT three ways in the next pass, each naming something the tokeniser had thrown away.
`**NOT/BLOCKING ...**` tokenised to `NOT`, `BLOCKING` and was accepted, because comparing a leading
token LIST discards everything BETWEEN the tokens. A bold run legally split across the line break
was invisible, because the head was a physical LINE and a bold run is delimited by its delimiters.
And a valid `BLOCKING` followed by a malformed `NOT BLOCKING_EXTRA` passed, because the
malformed-reach check was conditioned on no valid status existing — **one good declaration licensing
every bad one after it.** Three properties replace them, stated once each: the REGION is the item's
leading bold SEQUENCE, taken from the item body rather than a line; the PHRASE is matched ANCHORED
at the first run's start with one literal space, so nothing between the words is skipped; and the
POPULATION is every token in the region beginning with `BLOCKING`, each of which must BE `BLOCKING`,
checked unconditionally. `NONBLOCKING` does not begin with it and stays prose. **An unclosed run is
now read to its end rather than dropped** — dropping it was a fail-open I wrote into the repair and
then leaned on, telling myself the parity check covered it; parity does fire, but one refusal
standing in for two made the revert test unreadable, and it made one: reverting the head extent
blinded every status control at once instead of the one it should have.

**AN ITEM'S BLOCKING STATUS IS PARSED, not scanned, and that is a different property from being
parsed ONCE.** Five copies of the predicate became one at the predecessor SHA; codex-worker-1 then
broke the single copy three ways in one pass, and the three shapes name the three things a scan
lacks. `**BLOCKING, then NOT BLOCKING ...**` declared and retracted on one line, and the ambiguity
check counted only POSITIVE occurrences, so every consumer agreed — on the opening clause of a
contradiction. `**NOT BLOCKING-LIKE ...**` passed because `\b` sits between `G` and `-`, so the
scan read the head of a hyphenated word as the whole word. And an inline code span whose content is
the literal text `**BLOCKING**` supplied a status off raw bytes a reader sees quoted. **The repair is not a
fourth spelling patch.** The status is the FIRST BOLD RUN after the headline's `—`, read in the
VISIBLE view, and only its leading WORD TOKENS: token 1 is `BLOCKING`, or tokens 1-2 are `NOT`
`BLOCKING`, where a token is a maximal run of `[A-Za-z0-9'-]`. Including the hyphen in a word is the
one decision that makes `BLOCKING-LIKE` a different word without enumerating it, and there is no
window left to widen because nothing else on the line can be the status. A second status word
anywhere on the headline is a contradiction whatever its polarity. The code-span control had to be
DIFFERENTIAL: with the parser on the visible view a code span is simply blanked, so a control that
only ADDS one is a no-op and reports BLIND correctly — a repaired opt-out produces silence, not a
tag — and it therefore adds the span AND breaks the real status, so the old scan reads the span as
the status while the new parse names the malformed reach.

**Controls.** One hundred seventy, each naming the tag it must provoke; a control that mutates the file without
provoking its own tag reports `BLIND` and fails. The prose count and the names are themselves
checked against the harness, because this list said thirteen for two SHAs after the harness had
eighteen. **Run them ALL with `--sweep`**, which is a check and not a convenience: it asserts the
number of controls VISITED equals the number declared, and counts a control as fired only on a
FULLY ANCHORED match of a line the success path alone emits, with the control's own tag required
separately in the printed failure list. Both properties are repairs, and the second needed repairing
twice: its first version tested one success form by PREFIX while this paragraph claimed an exact
match, and built the other form's expected string out of the line it was testing. codex-worker-1
named both. The prefix arm was the one that opened holes — it accepted a bare `OK (+` with any
invented tail, and a genuine line with junk appended — while the self-derived arm made a component
vacuous rather than admitting a spoof. **An approximation reappeared inside the repair for a
self-certifying harness**, which is where this packet keeps finding them. As a shell loop the sweep
tested nothing and said so affirmatively — under zsh, which does not word-split unquoted
expansions, `for n in $NAMES` ran once with all the names as one argument; the gate refused
that correctly, but the refusal ECHOES the name it rejected, the rejected name was the whole list,
and the loop's predicate searched the run's output for substrings of that same list. It printed
`ALL 1 CONTROLS FIRED`. **A harness whose failure output satisfies its own success predicate**, with
the count `1` sitting next to the word ALL as the only tell. `--sweep` was then falsified twice: a
dead anchor turns every control red, and a single control given a tag nothing emits turns exactly
that one red while the rest stay green. Individual controls still run with
`--negative <name>`, list with `--list`: `status-malformed`, `status-ambiguous`, `status-contradicted`, `status-hyphen`, `status-code-span`, `item-md-unbalanced`, `item-md-view`, `ruling-item-state`, `ruling-cite-plus`, `unclassified-kinded`, `unclassified-said`, `opening-gates-gone`, `planning-unknown-gone`, `status-slash`, `status-wrapped`, `status-after-ok`, `item-md-escape`, `section-orphan`, `schema-unstated`, `ruling-state-adverb`, `ruling-state-cited`, `item-state-stale`, `owner-choice-said`, `owner-choice-gone`, `section-prefix`, `status-escaped`, `status-parked`, `restate-total`, `restate-open`, `choice-compound`, `status-parked-l2`, `status-entity`, `md-fabricated`, `section-dupe`, `item-state-cap`, `item-state-negelse`, `choice-adverb`, `status-litstar`, `status-entstar`, `status-parked-bad`, `section-indented-dupe`, `owner-choice-count`, `status-composed`, `section-recap`, `section-regate`, `blocking-negated`, `section-indented`, `header-suffix`, `g4-dup-quest`, `g4-dup-bang`, `g4-dup-terminal`, `g4-zero-terminal`, `g4-final-twice`, `g4-final-negated`, `g4-two-terminals`, `g4-final-rename`, `g4-dep-bareword`, `g4-dep-noclose`, `g4-dep-twolists`, `open-section-two`, `header-not-first`, `g4-dep-garbage`, `g4-dep-twophrase`, `item-dupe-number`, `g4-dep-dupe`, `g4-dep-owner`, `g4-dep-member`, `g4-dep-extra`, `marker-in-comment`, `g4-dep-count`, `marker-closer`, `marker-unclosed`, `edge-nospace`, `marker-qualified`, `marker-empty`, `edge-malformed`, `packet-marker-gone`, `packet-marker-added`, `packet-marker-dupe`, `packet-marker-typo`, `ruling-body-swap`, `closed-count`, `dangling-ref`,
`drop-refutation`, `member-dropped`, `member-per-type`, `open-arithmetic`, `open-count`,
`orphan-number`, `raw-without-cmd`, `rg-command`, `rule-arithmetic`, `stale-a0-sample`,
`blocker-set`, `borrowed-cmd`, `byhand-count`, `heading-regress`, `constraint-lost`, `label-spelling`, `manifest-stale`, `opening-gates`, `orphan-marker`, `two-markers`, `control-unlisted`, `no-terminator`, `handmade-count`, `root-wide-grep`, `ungated-ref`, `unmarked-popn`,
`unresolved-tail`, `unstated-return`, `withdrawn-claim`, `wrong-command`, `wrong-gate-ref`, `wrong-raw`, `drop-item-block`, `drop-gate-block`, `ruling-swallowed`, `restate-census`, `restate-census-i`, `restate-blockers`, `accept-prose`, `census-row-gone`, `census-relabel`, `census-cmd-swap`, `restate-r5-word`, `ruling-item-swap`, `dep-unknown`, `census-wrong-file`, `out-member-stale`, `out-writer-moved`, `dep-cycle`, `dep-self`, `census-fake-out`, `census-compound`, `dep-heading`, `dep-bad-token`, `ruling-long-head`, `ratchet-count`, `writer-extra`, `reader-row-moved`, `emit-fail-open`, `ruling-item-swap2`, `diagram-edge`, `control-phantom`, `writer-regroup`, `gate-multidigit`, `withdrawn-status`, `census-row-moved`, `writer-wrong-path`, `writer-path-prefix`, `writer-path-numeric`, `blocker-kind`, `edge-bad-source`, `md-outside-subset`, `fence-unclosed`, `fence-mixed`, `span-dbl-across`, `gate-wrapped`, `edge-second`, `marker-nonblocker`, `edge-bad-target`, `gate-bad-suffix`, `writer-bare-num`, `writer-num-suffix`, `writer-num-prefix`, `writer-in-comment`, `writer-wrapped-sep`, `writer-detached`, `control-dupe`. The last fifty-five exist because
**codex-worker-1** MUTATED THIS PACKET AND THE GATE STILL SAID PASS (the finding reached me relayed
by backend, and two commit messages in this lineage credit the relay rather than the author —
`e26f91f` and `c332c03`, immutable and wrong on this point): deleting the item's reopening sentence,
inverting input for output, changing R8's 7/8 to 70/80, R9's one to two and item 31's four to five —
every one of them survived. **Thirty-four controls were checking status ARITHMETIC and no control
was checking a substantive claim**, which is the difference between a gate and a gate's appearance.
`drop-item-block` and `drop-gate-block` close the first; the census block closes the numeric ones
by making them commands, and `restate-census` keeps a digit from reappearing in a ruling. Two of the controls were themselves defective when first
written and are recorded here rather than quietly fixed: `raw-without-cmd` changed a claim's NUMBER
and so provoked a different check entirely, and the landing assertion demanded the anchor count DROP,
which an insertion control can never do — it reported a landed mutation as unlanded. The named-tag
requirement is what exposed both.

# Owner rulings (claude-worker-2 as ADR/phase owner, at this SHA)

Five items were carried as "the owner SHALL rule", and two scope questions arose later. I am that owner; carrying them further would be
deferral wearing the costume of rigour. Each ruling states what it decides, why, and what it costs,
and each is a DECISION — none of them is evidence, and none may be cited as though a command
produced it.

**R1 — item 11 (G1-B) and item 25 (all): AUTHORED POPULATIONS, with drift detectors.** No predicate
distinguishes a request/answer reader from any other `read_` function, and none distinguishes a
REGION from the offset fields addressing it. Three attempts at a predicate produced 6 by hand, then
2 by intersection, then a withdrawal. The ruling is that these populations are **authored, not
derived**: each authored population must carry an EXPLICIT MEMBER LIST and a
DRIFT DETECTOR — a command whose figure is pinned, so that a change in the surrounding code
invalidates the authored list instead of silently outdating it. For G1-B that detector is
`grep -rn 'pub fn read_' ui/daw-bridge/src/control.rs` returns 21; if it ever returns anything else,
the authored list is stale by construction and the gate fails until it is re-authored.
This is weaker than a derivation and stronger than what preceded it, which was a hand selection
presented as a population. It also matches what the packet already does for the
hand-classified populations, so the two items resolve on one mechanism rather than two.
**APPLIED FOR G1-B, NOT FOR ITEM 25, and this clause has now been wrong in BOTH directions.** It
said NOT AT ALL for several SHAs after the population landed; I corrected it to say the DETECTOR
half was missing, and that was wrong too — G1-B publishes both, the seven-member roster enumerated
from the `Request*` enum and the two drift detectors beside it. **I wrote that correction after
grepping a line range that stopped short of the detectors**, which is the same mistake as the
original, one range further on. What genuinely remains is item 25 (all), whose six
hand-classified populations carry counts rather than member lists and need a predicate nobody has
proposed — and, for G1-B, the RUNNABLE CHECK asserting the specified detectors, which is item 33
(G1-B)'s and not this ruling's. codex-worker-1 found both wrong versions of this clause, and backend
found the third thing it conflated. The reviewer found
this by reading R1 against the gates rather than against itself, which is the right test and one I
did not run: I wrote "the packet lists the members explicitly" in the same commit in which it did
not. **Cost:** a reviewer must read the lists, not re-run a command; the detector bounds staleness,
not correctness. What remains under this ruling is filed at item 25 (all); the G1-B half is
discharged, and any further detector work there is item 33 (G1-B)'s, which is where S4 puts it.

**R2 — item 18 (G2-B): TWO-LEVEL READINESS, not a recovery-only exemption.** Readiness is staged —
`mapped-and-bypassed` and `mirror-complete` — and dispatch is permitted at the lower level. The
exemption was the smaller change and is the worse one: it creates a dispatch path that exists only
during recovery, which is the least-exercised state in the system and the one that runs when
something has already gone wrong. A LEVEL is a value that every dispatch can carry and every
assertion can read; an EXEMPTION is a rule that lives in whichever branch remembered it, which is
this repo's documented failure shape. **Cost, stated because it is real, and PROPAGATED at this SHA:**
G4's dispatch identity carries the readiness level: the identity is a
QUINTUPLE and PASS 5 compares five components. G2-B's invariant permits dispatch at
`mapped-and-bypassed` and requires `mirror-complete` only for processing that depends on mirrored
parameters. **That propagation is DONE at this SHA** — it was described here as pending for three
SHAs while I wrote notes about it, and a ruling that contradicts the gates it governs is worse than
an open question, because a reader can satisfy the gates and violate the ruling. What this ruling
does NOT deliver is the IMPLEMENTATION of the two levels in the product, which no packet edit can
reach; that work is filed at item 18 (G2-B), and its state is read there.

**R3 — item 19 (G3): N IS AUTHORED AT 3, and the packet says authored, not derived.** Nothing in
the tree sources an observation count; `hardTimeoutBlocks = 500` is a block count and converting it
would be inventing a rate. So N is a design constant: **a host must fail to advance across 3
consecutive observations before eviction, and 3N = 9 observations of absence**. The constraint the
value must satisfy is stated so a successor can rule differently on evidence: N observations must
be long enough to survive ordinary scheduling jitter and strictly shorter in wall-clock than the
existing hard timeout, so containment happens before the blunt instrument fires. **No static check pins the literal at this SHA**, and one is
required: without it N = 3 lives only in this prose and a change to the implementation would not be
visible to any gate. That check is part of item 19's ticket, and the packet says the check is absent
rather than describing it as though it exists. **Per backend's direction at
this SHA**, R3 makes G3 decidable FOR IMPLEMENTATION PLANNING in the same way R1 does for the
authored populations: N = 3 is an authored parameter, not a derived fact, and the work it implies is
a ticket — pin N with its units and semantics, instrument the production watchdogs, define drift and
measurement acceptance, and have it independently validated. That work is filed at item 19 (G3),
which this ruling does not discharge.
G3 is NOT permanently resolution-blocked and must not be classified as such. **Cost, and a correction to this ruling's
own reasoning:** I first justified 3 as "the smallest value that is not 1". Two is. The reviewer
caught it and the justification is replaced rather than patched: 1 evicts on a single unlucky
sweep, and 2 evicts on any two adjacent sweeps, which one scheduling hiccup spanning a sweep
boundary produces — 3 is the smallest bound that requires a host to miss a sweep it had a full
interval to make. That is an argument, not a measurement, and it is offered as a starting point
measurement should overturn. A ruling whose stated reason contains an arithmetic error is worth
less than the number it defends, so the error is recorded here rather than silently corrected.

**R4 — item 24 (G0-B): THE RUST SIDE RENAMES.** `patcher_abi.h:75` keeps `reserved`;
`patcher_rust/src/lib.rs:86` changes `_pad0` to `reserved`. The C++ header is the ABI AUTHORITY —
renaming the authority to match its mirror inverts which document
defines the contract. **Correction to my own reasoning as published:** I wrote that bindgen consumes
the header through an `allowlist_file` closure. It does not — there is no `build.rs` under
`patcher_rust/` and no `allowlist_file` in the tree. The ruling stands on the weaker and true
premise that `patcher_abi.h` is where the C++ side of the ABI is DECLARED and the Rust file mirrors
it, but a reader should know the generation story I gave was wrong. The convention argument cuts the
other way (`_pad0` is idiomatic Rust) and loses to that: a binding may be unidiomatic, an authority
may not be derived from its binding. **Cost:** one unidiomatic name in the Rust file, and PASS 9
goes from RED to decidable the moment the rename lands.

**R5 — item 26 (G4): THE IN-SCOPE POPULATION IS BYTE-CONSUMING READS, the whole census row and not a
subset of it. SUPERSEDED IN SCOPE BY R8(c), which is stated here rather than left for a reader to
infer from two rulings that disagree.** R5 selects the READER role; R8(c) rules that the gate ranges
over readers AND writers, because an ordering invariant is a relation between a write and a read and
a gate enumerating one end can never observe the pair. So R5 is operative for what a READER is and
superseded for what the GATE ranges over. codex-worker-1 found both standing unmarked, each correct
alone and jointly unsatisfiable — the pattern this packet has hit three times, and the repair is
always to say which one yields where rather than to soften both.** With
the reader table above HAND-CLASSIFIED — not derived; the manifest marks that row
`derived: false` and this sentence said otherwise — this ruling now selects a named set rather than
a category: readers
1-7, main plane and aux, direct and helper-mediated and callee-deref alike. A reader does not leave
the population by reaching the bytes through a helper; the invariant is about consuming bytes
another agent wrote, and the route is irrelevant to that. **This resolves the R5/R7 relationship
codex-worker-1 asked about:** R5 selects the ROLE (byte-consuming reads), R7 fixes the SCOPE (the
host is an agent within it), and they are orthogonal — R7 widens which agents count, R5 narrows
which sites within them do. **SUPERSEDED TAIL, recorded as history and no longer as rule.** This ruling used to continue: that
the population the GATE ranges over is the sites dereferencing the out-plane for sample data, that
byte-PRODUCING writes are out of scope, and — stated as a cost — that the gate therefore does not
govern the write side. **R8(c) rules the opposite and is operative**: the gate ranges over both
endpoints, because an ordering invariant is a relation and a gate holding one end cannot observe the
pair. The paragraph above kept saying otherwise in the present tense while carrying a SUPERSEDED
label at its head, which is a document stating a rule two ways — the failure this packet has now
made four times, and marking the heading is not marking the text. What survives from that tail is
the point about PARTITION: establishing sites and writes must still appear in the partition even
where they are outside a role, because a partition that drops a class cannot be checked for
totality, and that is how an earlier attempt here asserted a sum of 27 over terms adding to 20.

**R6 — item 27 (G2-A): THE GATE RANGES OVER THE REFUSAL-EMITTER POPULATION.** G2-A is about command
identity through a refusal — a UI adopting an engine refusal as the outcome of the command it sent —
so the governed set is the emitters of an adoptable refusal, and the version-arbitrated set is a
RELATED population it must not be confused with. Every control states which one it ranges over.
**Cost, and it is the whole reason this ruling is needed:** the two sets differ in both directions,
so neither can stand in for the other. A control written against "arbitrated" while claiming
"refusable" is the error this packet published and retracted.

**R7 — item 26 (G4): THE HOST PROCESS IS IN SCOPE.** G4's invariant is a consumer reading bytes
ANOTHER AGENT wrote, and the other agent is `juce_host_process_main`. A scope that excluded it would
leave the gate unable to see the write side of its own invariant — and it already does not see it,
because the population's four name-terms miss the host's spelling entirely. Note this differs from
G1-A, whose RAW deliberately excludes `juce_host_process_main` because that gate is about the
engine's own ring; a per-gate scope is not a packet-wide one. **Cost:** the population had to be
rebuilt by derivation rather than by matching spellings, which is a harder census than a grep. **That
census is DONE, and this clause named the wrong derivation for four SHAs.** It said the rebuild must
key on `ShmHeader::audioOutOffset` — the shape the G4 population passage REFUTES, because the host's
aux offset is a running accumulator descended from that field and a census keyed on the field name
misses it for exactly the reason the four-spelling census did. The implementable census keys on the
MAPPED BASE and is the complete role census in G4. Remaining work is filed at item 26 (G4).

**R8 — item 26 (G4): THE THREE ROLE RULINGS claude-worker-1 correctly refused to make.**
**(a) SAME-AGENT READBACK IS OUT OF THE POPULATION, and named rather than dropped.** `juce:834` and
`:1015` are the writer reading bytes it wrote itself. G4's invariant is a consumer reading bytes
ANOTHER AGENT wrote; a self-read has no cross-agent ordering hazard because there is no
acknowledgement to wait on. They are byte reads of the plane, so they are listed as EXCLUDED
MEMBERS with this reason — an exclusion by argument, not by the selector failing to see them.
**(b) THE INDIRECT WRITER IS ONE SITE, AND IT IS THE HOST'S.** `juce:989/994` hands `outputPtrs`
and `auxOutputPtrs` to the plugin, which writes them. The plugin is third-party code this packet
does not govern; the host is accountable for what it hands over, so the site is the handover.
Attributing it to the plugin would put a member of G4's population outside anything the packet can
require.
**(c) THE GATE RANGES OVER BOTH SIDES: the CENSUS rows *out-plane cross-agent byte-consuming reads*
AND *out-plane host byte-producing writes*.** This ruling carries NO DIGITS, by rule and by check
(`CENSUS-RESTATED`): backend changed a `7/8` written here to `70/80` and the gate passed, because a
number restated in a ruling is a second statement of a fact the census already owns and nothing
compares them. The fix is not a comparison — it is that there is nothing here to mutate. R5 named
the readers, and a reader set alone cannot check an ORDERING invariant — "no consumer reads before
the acknowledgement" is a relation between a write and a read, and a gate enumerating only one end
can never observe the pair. This corrects R5 rather than extending it: R5 said "the in-scope
population is byte-consuming reads", and that was half a population for a two-sided invariant.

**THE COMPLETENESS ARGUMENT, which is what this population actually needed and what NEITHER census
gave alone.** A mapped-base census is not total either, for the mirror of the reason a name census
is not: four of the seven readers never cast a base themselves — they take a pointer from a helper,
and the census collapses them into the helper unless callers are followed. There are exactly THREE
ways a plane address can arise:

    A  a direct cast of the mapped base                     5 sites
    B  the return of `audioOutChannelPtr` (`audio_shm.cpp:5-12`)      caller: juce:638
    C  the return of `safeAudioOutPtr` (`engine_produce_block.cpp:861-866`)
                                                            callers: :1030, :1112, :1150

**The two helpers are G4's, and the FILE has four.** `audio_shm.cpp:5` / `:17` and
`engine_produce_block.cpp:861` / `:848` are two symmetric pairs, one per plane —
`audioOutChannelPtr`/`audioInChannelPtr` and `safeAudioOutPtr`/`safeAudioInPtr`. G4's argument is
unaffected because its population is the OUT plane and the two out-plane helpers are the two named;
but "exactly two pointer-returning helpers" is a claim about the FILE and is false about it, and
claude-worker-1 tested their own premise and said so before it hardened into one.

**The argument:** a site can only touch plane bytes by holding a plane pointer; a plane pointer can
only be obtained by casting the mapped base or by calling one of the two functions that return one;
all three sets are enumerated. That is an argument from the code's STRUCTURE rather than from a
grep's recall, and it is FALSIFIABLE — add a third pointer-returning helper and it must be extended.
The offset-returning helpers (`auxOutputPlaneOffset`, `audioChannelOffset`) feed route A and are
already counted there.

**R9 — item 26 (G4): THE POPULATION COUNTS ADDRESSING SITES, and consumption sites are named
separately.** `engine_audio_callback.h` contains exactly ONE segment-base dereference, at `:404`;
`:448` and `:463` are the two places `trackChannel[i]` is CONSUMED — the PDC fast path and the PDC
delay path. Both descriptions are correct and they count different things, which is why the packet
must say which: the reader that `engine_audio_callback.h` contributes is a single member on
addressing sites and a pair on consumption sites, and the row total moves with the unit chosen. **The gate counts ADDRESSING sites, because the invariant is about
acquiring a plane address and reading through it before an acknowledgement.** But the two
consumption sites are named, because **a fixture that exercises only the fast path never touches the
delay branch** — which reads through a ring one block behind, and that is precisely the shape a
barrier test should target. A population that hid the delay path behind its addressing site would
have made the fixture look complete while missing the harder case.

**THE RATCHET THIS ARGUMENT REQUIRES, and item 31 carries it:** the completeness argument rests on
"exactly two pointer-returning helpers", which is itself a census and deserves a check rather than
anyone's word.

**A THIRD FRAGILITY, named by claude-worker-1 and not by any check here.** `engine_produce_block.cpp`
has THREE bindings spelled `input` — `:535` and `:817` are `const float*` aimed at other buffers, and
only `:945` is the plane. The engine-write row greps the FILE for the name and returns exactly the
eleven that belong to `:945`'s scope, so it is right at this pin and right for the wrong reason: it
would absorb a write through either other binding the moment one stopped being const. A row bounded
by a name inside a file with three same-named bindings is a floor, and the row that stated the
binding as its rule is the one that found the eleven.

**The two OUT rows are the ones a reader should distrust**, and stating 4 against a claim of seven
and 13 against a claim of eight is the point: the gap IS the hand classification, and a row that
quoted a pattern returning exactly the claimed number would be a pattern reverse-engineered from the
answer. The SEVEN IN rows have no such gap because the roles there are separable by shape —
seven, not the five this sentence said after the unit split added two, which is the same stale-count
shape the checks below exist for and which no check reaches inside a prose paragraph.

**THE ALIAS CHAIN IS THE FINDING, and it is the same defect this population has now made four
times.** The host reaches the input plane through TWO hops: `state.inputPtrs` → `inputPtrs`
(`:862`) → `pluginInputPtrs` (`:880`). Five of the byte reads dereference the SECOND alias
(`:924`/`:927`/`:930` the level-matched bypass copy, `:939`/`:942` the bypass meter, `:975`/`:980`
the input meter) and a census over the member name `state.inputPtrs` returns FOUR sites and sees
none of them. claude-worker-1's input census reported five engine writes in this file; the file holds
ELEVEN, because the window started at the visible branch (`:1016`) while the binding that governs
every write is `float* input = safeAudioInPtr(blockIndex, ch)` at `:945` — the sampler-stem,
sidechain and sampler-audio branches at `:967`–`:1017` write the same plane through the same
pointer. Eleven here plus the master summed-mix write is twelve engine writes in all, and those are
TWO census rows rather than one figure: writing "twelve writes through two addressing routes" as a
single number, as this paragraph did, is the conflation the rows exist to prevent. **A census
is bounded by the BINDING, never by the branch you were reading.**

**`engine_produce_block.cpp:1032` IS A MEMBER OF BOTH RELATIONS AT ONCE.** `std::memcpy(input, output, ...)` is an out-plane byte read and an in-plane byte write in a single statement. Any partition
into "the readers" and "the writers" either double-counts it or drops it, and two controls ranging
over the two lists would each claim it. It is named once here, in both roles, deliberately.

**HOST ROLES, and one is INDIRECT.** `:686` (aux copy) and `:720` (passthrough) read through
`state.inputPtrs` directly; `:685` is a null-and-size guard and reads no bytes; `:644` establishes
the address. `:987` hands `pluginInputPtrs` to the plugin, which reads it — the same treatment R8(b)
gives the indirect WRITER, and for the same reason: the plugin is third-party code this packet does
not govern, so the host's handover is the accountable site.

**R10 — item 30 (G2-A): THE UNDO ARBITER IS DEAD WIRING, THE HAZARD IS REAL, AND THE INSTRUMENT IS
WRONG FOR IT.** Measured before ruling. `handleUndo` (`engine_undo_commands.cpp:46-72`) calls
`deps.applyDocument(doc)` — it replaces the ENTIRE `ProjectDocument` from history, then restores
plugin state. A per-clip `requireMatchingClipVersion` cannot express "the document you are undoing
from is the one you saw": it arbitrates ONE clip's version while undo swaps everything.
**So the `std::function` member at `engine_undo_commands.h:38` is dead wiring — remove it.** Keeping
an unused gate wired in suggests to every reader that arbitration happens here, which is worse than
its absence: it is a check that exists in the type system and nowhere in the behaviour.
**And the hazard it appears to cover is REAL and uncovered**: undo applies a whole document, so an
edit made between the user's view and their undo is silently reverted. Closing that needs a
DOCUMENT-level version, which does not exist at this SHA. Item 30 records the removal and the
hazard separately, because deleting the wiring must not read as dismissing the risk.

**R11 — item 32 (G2-A): `commandMutatesDocument(Undo)` RETURNS FALSE WHILE UNDO REPLACES THE
DOCUMENT.** `engine_command_mutates.h:58` classifies `Undo` as "no document state" and returns
false; `handleUndo` calls `applyDocument`. The classifier is answering "does this command CARRY
document state in its payload" and its name asks "does it MUTATE the document" — and G2-A's
arbitrated population is derived from that classifier. A command that replaces the entire document
while classified as not mutating it is a live inconsistency in the predicate this gate depends on,
not merely a misleading name.

**R12 — item 27 (G2-A): THE GATE RANGES OVER THE ADOPTABLE-REFUSAL EMIT SITES, and the reason the
old scope kept falsifying is that THERE ARE THREE REFUSAL CHANNELS, not one.** The population is the
production `emitClipReject` sites — RAW 4 (`git grep -n -E '^ +emitClipReject\(' apps`) → minus 1 (`engine_ui_publish_tests_main.cpp:198`, a test main) → **3 IN SCOPE**:
`engine_clip_edit.cpp:965` (UnknownTrack), `:986` (StaleBase) and `engine_rowops_commands.cpp:49`
(SetRowOps). ⟂

**The three channels, each with a different adoptability.**

    ClipRejected                 requireMatchingClipVersion (the DEPS one) -> emitClipReject.
      command-specific           `UiClipRejectPayload` carries **commandType** — it names what was
                                 refused. Reached from 6 arbitration call sites
                                 (`git grep -n -E 'requireMatchingClipVersion\((auto|chord|diff|h|p|payload)' apps | wc -l` returns 6)
    UiDiffType::ResyncNeeded     daw::requireMatchingClipVersion (the FREE one, `clip_edit.cpp:5-15`)
      terminal, not addressed    fills a diff with `clipVersion = currentVersion` and NO command
                                 field
    UiHarmonyDiffType::           HarmonyTimeline::requireMatchingHarmonyVersion
      ResyncNeeded               (`engine_harmony_timeline.cpp:111-120`) sets
      terminal, not addressed    `harmonyVersion = current`, no command field. Reached from
                                 `engine_note_commands.cpp:93` and `:112` — WriteHarmony and
                                 DeleteHarmony, which never reach either clip arbiter

**A CORRECTION TO THIS RULING'S FIRST STATED REASON, which was false at the pin.** R12 originally
excluded the two ResyncNeeded channels because they "carry no retry value, nothing to adopt".
**They both carry one** — `diffOut.clipVersion = currentVersion` at `clip_edit.cpp:13` and
`diffPayload.harmonyVersion = current` at `engine_harmony_timeline.cpp:119`. codex-worker-1 caught
it. The real distinction is that a `ClipRejected` NAMES A COMMAND and a ResyncNeeded diff does not:
one is addressed to a caller, the other is a terminal "you are stale" that any reader may pick up.
The exclusion stands and its reason was wrong, which is worth more than a quiet edit — I authored a
ruling on a property I had not read the payload for.

**WHY THE EMIT SITES AND NOT THE ARBITRATED COMMANDS.** G2-A's invariant is that a refused command
tells the UI what to retry with. That is a property of the EMIT, not of the check. Selecting on
arbitration includes commands whose refusal the gate cannot test — the two ResyncNeeded channels
carry no retry value — and EXCLUDES a site that fails the invariant, because `SetRowOps` emits an
adoptable refusal while being arbitrated by nothing: `UiSetRowOpsPayload` has no `baseVersion` field
at all. **Every previous scope here was falsified by the same move: it selected on the check and the
gate is about the answer.** The containment claim B = A ∪ {SetRowOps} failed for the same reason —
the two sets are not nested because they are indexed on different things.

**ALL THREE MEMBERS FAIL THE INVARIANT, not one — and that correction is codex-worker-1's.** I wrote
that `engine_rowops_commands.cpp:49` was the single failing member because it passes
`/*sentBase=*/0, /*currentBase=*/0` — constants, where the other two pass real values and zero is a
live `clipVersion`. That is true and it is not the whole defect. **`commandType` names a KIND, not
an INSTANCE**, and `UiClipRejectPayload` carries no sender-minted per-command identity at all
(`event_payloads.h`: diffType, reason, trackId, sentBase, currentBase, commandType, reserved). The
consumer correlates on `(track, commandType, sentBase)` — `ui/daw-cli/src/main.rs:1179`, live, with
four call sites of `await_clip_outcome` at `:2989`, `:3000`, `:5418`, `:5430` — so two concurrent
refusals of the SAME command type on one track at the same base are indistinguishable for every
member of the population, not just for the one that zeroes its bases. rowops:49 is additionally
worse, not uniquely wrong.

**A population all of whose members fail is the right population.** That class-wide defect is open
item 27 (G2-A)'s OWN — item 29 (G2-A) is only the narrower `SetRowOps` constant-base case, and this
sentence attributed the broad defect to it for four SHAs after codex-worker-2 ruled otherwise and
after the edge was removed from the item. **Correcting the item and leaving the ruling is the same
half-fix as correcting the ruling and leaving the item**, which this packet did in the opposite
direction three SHAs ago. A population defined to exclude its own failing members would be a
population chosen to pass, and one narrowed to a single failing member would have understated the work by two thirds.

**R13 — item 34 (G2-B): RETRACTED. I inferred a guard's purpose from its NAME and both halves of
the ruling are false.** R13 claimed `mirrorOnly` was a consumption boundary keyed one event too
early. codex-worker-1 refused it and measuring settles it against me:

    mirrorOnly is read at exactly TWO sites, `engine_produce_block.cpp:602` and `:620`, both
    `if (!mirrorOnly && isPlaying)` — renderTrack and note emission. `sendProcessBlock` at
    `:1075` and `:1089` is OUTSIDE any mirrorOnly guard.

**So `mirrorOnly` is not a discard boundary at all** — it suppresses track rendering and MIDI while
a mirror is pending-and-unprimed, and the block is dispatched and consumed regardless. My ruling
opened with "one exists", and one does not. And the hazard it was built on does not follow either:
`writeMirrorParams` (`engine_ui_publish.cpp:92-130`) stamps each mirrored param into the SAME
time-ordered event ring as the block, at the priming block's own `sampleTime`, so the params are
drained in order before that block's DSP rather than racing it.

**The error is one I wrote a note about earlier in this same session and then repeated twice.** I
reasoned from a name — `mirrorOnly` sounds like a mode that suppresses everything — instead of
reading the statements it guards, exactly as I reasoned about `ResyncNeeded` from its channel name
instead of its payload, and about `EventEntry` from a struct in the wrong crate. **The fix that
would have caught all three costs one command: read the guarded region, not the guard's name.**

Item 34 is WITHDRAWN. The proposed one-condition change would also have been wrong on its own terms
— `mirrorPending` is a per-track flag consulted in a loop over all tracks, so gating on it globally
would silence unrelated tracks' timeline events. **Do not implement it.**

What survives as a real question, and it is codex-worker-1's rather than mine: G2-B's PASS 4 is
still withdrawn with no replacement while R2 says propagation is done, and no PASS condition accepts
the two-level implementation that exists. That is item 18's, where it already lives.

**R14 — item 35 (G0-B): EXACT MEMBER PARITY IS REQUIRED FOR `EventEntry`, and PASS 8's
byte-disjointness exception does not reach it.** G0-B's invariant demands per-member parity between
the C++ and Rust declarations of a shared type; PASS 8 permits a one-sided member where the bytes
are provably disjoint. Those two are not in conflict in general — a member one side never touches is
genuinely harmless — but they cannot BOTH be satisfied by this pair, and item 35 named the mismatch
without saying which rule wins.

**Ruling: parity.** The exception is about bytes NEITHER side treats as live data. `ready` is
written and read as the multi-producer publication flag on the C++ side, so disjointness would
require the Rust side never to write [60,64) — and `*slot = entry` is a whole-object store whose
width is `size_of`, which includes the padding. **A byte-disjointness argument that depends on a
compiler's discretion about padding is not a disjointness argument.** Whatever fixes item 35 must
make the two declarations agree member-for-member: declare `ready` on the Rust side, or give the
patcher path a distinct transfer type that is not claimed to be the same ABI. A field-wise copy
alone does NOT satisfy this — it would leave 7 members against 6 and the invariant unmet, which is
the trap codex-worker-1 named.

**R15 — item 36 (G3): G3's M1 IS DEFINED HERE, AND ITS CENTRAL CLAIM IS RETRACTED — the check is
NOT the stale side, the mechanism is NOT statically decidable, and M1 or an equivalent measurement
IS owed.** G3's Deterministic-test block names Layer 0, Layer 1 and
Layer 2 and never defines an M1; PASS 1 and the register then both refer to one, borrowing G0-B's
M-label convention (`:335`) across a gate boundary that does not carry it. **M1 is hereby Layer 0's
mechanism**: run `tools/host_stall_check.sh` against the pinned tree with `DAW_ENGINE_DEBUG_STALL`
set, and observe, WITHIN the SIGSTOP window the script opens at `:85` and closes at `:92`, whether
the frozen host still owes dispatched blocks and whether the producer is gated on it. PASS 1's
refutation clause is right about the window — after `kill -CONT` at `:92` the host is running
normally — and it stays.

**M1 CANNOT BE BUILT FROM THE INSTRUMENTS I FIRST NAMED.** I defined M1 as recording (a) `kill -0 $FROZEN` and (b) the armed stall log's
per-host `hosts=[…]` vector. Neither carries the needed fact. `kill -0` proves a process EXISTS; a
SIGSTOPped process and a running one answer it identically, so it cannot establish the
alive-but-stopped state I claimed for it. And the per-host vector is positional and thin: `:262-266`
emits `active` as a two-way separator and `completedBlockId`, with **no track or PID identity and no
`lastDispatchedBlockId`** — so it cannot evaluate the owes-nothing predicate at
`apps/engine_rt_helpers.cpp:502`, which is the entire question, and it cannot even say which entry
is the frozen host. codex-worker-1 caught both. **I specified an oracle by confirming a log exists
and never reading what it emits**, which is the same shape as a guard that is present but watches the
wrong moment. A usable M1 needs a channel that carries `lastDispatchedBlockId` and host identity, and
building one is PRODUCT work that this packet does not authorise.

**WHICH LEAVES M1 WITH ONE STATUS, STATED ONCE, because three sentences in this packet gave it
three.** R15 said the register's demand for M1 "survives", item 36 said it "remains owed", and the
register records R15 as having discharged the decision — while R15 itself calls the remaining
question debugging rather than acceptance. codex-worker-1 named the incoherence. The single status
is: **M1 OR AN EQUIVALENT MEASUREMENT IS OWED, AND CANNOT BE BUILT FROM TODAY'S TELEMETRY.** An
earlier version of this paragraph said it was neither owed nor acceptance evidence, on the strength
of the static settlement retracted above; with a real drop path in the tree, nothing static
distinguishes "gated and stayed gated" from "gated, dispatched, send failed, host dropped". That
distinction is the register's question. The instrument needs host identity and
`lastDispatchedBlockId`, which nothing currently emits, so G3 is blocked on an INSTRUMENT rather than
a decision. **An earlier draft added "and no PASS bullet may depend on M1 until one exists", which
G3's PASS 1 contradicts directly and which is wrong**: Layer 0's question genuinely needs a
measurement, so its bullet must name one. What follows from an unbuildable instrument is that the
GATE cannot be ACCEPTED, not that its bullets must pretend to need nothing.

**The two statements.** The check says the producer does not stay gated
(`tools/host_stall_check.sh:16-19`), names the mechanism at `:101-104` — "Frozen host + fix = the
host is dropped and production continues" — and makes it EXECUTABLE at `:105-109`, failing when more
than three `producer stall (inFlight)` lines follow its in-log marker. The producer comment the
register asks for is `apps/engine_rt_helpers.h:232-234`, which says the opposite while naming the
check: "a stopped host still OWES its dispatched blocks, which is the one case where holding the
gate shut is the correct answer."

**The mechanism half is decidable at the pin, and the check is wrong.** `completedMinimum`
(`apps/engine_rt_helpers.cpp:492-511`) excludes a host on exactly three conditions — `!active`
(`:493`), `completedBlockId == 0` (`:496`), and owes-nothing —
`lastDispatchedBlockId > 0 && completedBlockId >= lastDispatchedBlockId` (`:502`).
A SIGSTOPped host that had been producing
satisfies none: its completed id is non-zero, and its dispatched blocks are precisely what it has
stopped finishing. It is counted at `:509-510`, holds `lowest` at its frozen id, and
`apps/engine_producer_thread.cpp:282-283` gates. While gated the producer dispatches nothing, so the
frozen host's completed id cannot advance — it is stopped, and nothing else writes it. Its
`lastDispatchedBlockId` is NOT equally frozen: a later dispatch can update it
(`apps/engine_produce_block.cpp:1072-1108`), so "neither id can move again", which an earlier draft
said, is wrong in the second half. **An earlier draft called the state ABSORBING and that is
not statically established** — codex-worker-1 pointed at `:232-233` and `:236-237`, where a failed
`controllerMutex` try_lock or an absent mailbox `continue`s past a host, so WHICH hosts are counted
varies between iterations and the observed minimum can change with no block id moving at all. The
claim that survives is narrower and sufficient: the frozen host cannot advance on its own. The first exclusion never participates at all —
the engine's only caller builds every entry as `hostProgress.push_back({true, completed, …})`
(`apps/engine_producer_thread.cpp:249-251`), so `active` is hardcoded and `!active` is unreachable
from production; the `":inactive@"` branch in the same function's log (`:265`) prints for a state
that cannot occur. Nor is there a liveness drop to appeal to: no `kill(pid, 0)`/`ESRCH` test exists
under `apps/` outside test mains and teardown.

**AND THAT CONCLUSION IS WRONG, WHICH RETRACTS THIS RULING'S CENTRAL CLAIM.** An earlier draft said
"the host is dropped names nothing in this tree" and ruled the check the stale side. **A drop path
does exist**: `apps/engine_produce_block.cpp:1096-1100` stores `hostReady = false`, `active = false`
and `needsRestart = true` when a send fails, and `apps/engine_consumer.cpp:840-841` schedules the
restart. It is not a liveness probe — which is why looking for `kill(pid, 0)` found nothing and I
took absence of the mechanism I EXPECTED for absence of any mechanism. It is reached by a failed
dispatch, and the try_lock/mailbox omissions above are exactly what can let a dispatch be attempted
while the host is stopped. So an owing host can gate first and be dropped afterwards, which is a
coherent reading of the check's comment rather than a fiction. codex-worker-1 found it.

**This ruling's own document already carried the evidence.** G3's failure model cites
`apps/engine_produce_block.cpp:1096-1101` by line as the per-track detector that "clears `hostReady`
on `!sentOk`". I wrote a categorical absence claim two screens below a sentence naming the thing I
said was absent.

**What survives.** Readings 4 and 5 still hold: the check cannot OBSERVE whether the producer was
gated, so it is non-discriminating whichever mechanism operates. What does NOT survive is that the
register's question is settled statically — it is not, and M1 or an equivalent measurement IS owed
after all. The single status recorded below is corrected accordingly.

**A CORRECTION TO THIS RULING'S OWN EVIDENCE, and it is the same error twice in one day.** The
sentence above first cited `apps/engine_ui_thread.cpp:57` and `:77` as the only writes of
`.active = false`, offered as proof that no host is ever dropped. Those two lines belong to
`struct StallWatch` (`apps/engine_ui_thread.cpp:51`), a UI ring-recovery type with no relation to
`HostProgress`. codex-worker-1 caught it. I had grepped a MEMBER NAME across a directory and taken
the hits without reading their type — the identical mistake as answering a question about
`patcher_rust/src/lib.rs` by measuring `ui/daw-bridge/src/layout.rs`, which is recorded three items
above this one. **A grep for `.active` is a grep for a word, not for a field**, and the conclusion it
supported happened to be true, which is exactly why it survived a re-read. The claim now rests on the
construction site, where the value is visible.

**What that does NOT settle.** The check's other half is not prose, it is an assertion — and if the
producer comment is right, that assertion should be failing. **I gave three readings and said no
static reading could decide between them. codex-worker-1 supplied a fourth and a fifth, and both are
decidable at the pin**, which also refutes the "no static reading can" claim I attached to item 36:

1. the check does not run;
2. it freezes a host the gate was never going to count — `:82-83` picks by `pgrep`, preferring
   `_0.sock` and otherwise the first match, with no test of membership in the counted set, and
   exclusion `:496` makes this concrete for any host that has completed nothing;
3. something outside `completedMinimum` intervenes;
4. **the marker is overwritten before `awk` can see it.** The engine's output is
   `>"$TMP/eng.log"` inside a subshell (`:65`) — one open file description the engine holds for its
   lifetime — while `froze-host-marker` is appended through a SEPARATE description at `:89`. The
   engine's offset does not advance when another descriptor appends, so its next write lands at its
   own offset and can overwrite the marker. `:105` then never sets `seen`, `n` stays 0, and
   `STALLS_AFTER=0` passes;
5. **the gate holds but says nothing.** `cli do play --force … || true` (`:77`) discards a failed
   play, and at `apps/engine_producer_thread.cpp:283-289` only the `logStall` call is wrapped in
   `if (isPlaying)` — the `continue` at `:289` is OUTSIDE it. **So a non-playing producer stays
   gated exactly as before and emits no `inFlight` line at all.** The check counts log lines, so it
   reads a gated producer as zero stalls, whether or not the gate is still held.

Readings 4 and 5 are two independent channels by which the check returns 0 WITHOUT OBSERVING whether
the producer is gated, and neither needs a bypass in the engine. **They show the check is
non-discriminating, not that the gate is held forever** — an earlier draft said "gated forever",
which claims a perpetual population membership nothing here establishes and which R15's own
try_lock/mailbox paragraph contradicts. **They do NOT dissolve the contradiction, and an earlier
version of this paragraph said they did.** They establish that the check cannot tell its own answer
apart from a false green — which is a defect in the check — while leaving the register's question
open, because the drop path the check's comment names is real. Which mechanism actually operates in a
given run is therefore an ACCEPTANCE question for this gate, not a debugging one, and it needs a
measurement nothing currently emits.

**Recorded next to both, as the register requires.** The two addresses are
`tools/host_stall_check.sh:16-19`, with `:101-104` carrying the mechanism claim that fails, and
`apps/engine_rt_helpers.h:232-234`. Both sit in PRODUCT, which I do not edit; the ruling is recorded
here against both addresses and item 36 carries the product-side correction.

**R16 — item 37 (G3): RETRACTED. I ruled on a mechanism this gate explicitly forbids, and what
survives is PASS 7's missing rationale rather than a defect.** R16 argued that evicting a host which
still owes dispatched blocks removes the invariant making `blockId % numBlocks` unambiguous, and
derived a corrupt read, a corrupt publish and a returning deadlock. Every step assumed eviction means
REMOVAL FROM THE PRODUCER'S MINIMUM. **G3 does not define it that way, and says so twice**: the
Invariant requires the host to have `hostReady` stored false by an autonomous engine path
(`:961-965`), and PASS 7 reads "The eviction lands on `hostReady`, not merely on the producer's
vector" — naming the producer-vector-only form as its own refutation. I ruled on the shape the gate
rules out.

At the pin, `hostReady == false` closes all three doors R16 needed open. The producer skips the host
at `apps/engine_producer_thread.cpp:226-227`, before it can enter `hostProgress` at all, so it cannot
hold the minimum. `processTrack` returns at `apps/engine_produce_block.cpp:362-363` before any newer
input or output is written to that track's planes, so a resumed host reads its OWN block's input. The
mixer skips the track at `apps/engine_audio_callback.h:305-306`, so a late write cannot be played.
And the frozen host is never readmitted in place: the production `hostReady.store(true)` that
follows an eviction is `apps/engine_restart_worker.cpp:87`, immediately after
`runtime->controller.launch(…)` (`:70`) — a RELAUNCH with a new child. SIGCONT alone cannot bring the
stopped process back into the counted set. codex-worker-1 established all of it.

**THE RETRACTION GOES THIS FAR AND NO FURTHER, and an earlier draft of it overreached.** I wrote that
relaunch "closes all three doors", which is not established, and codex-worker-1 named the gap on the
next pass. `EngineAudioCallback::TrackInfo` holds the MAPPING as a snapshot —
`std::shared_ptr<const daw::SharedMemoryView> shmView` with `shmBase` and `header` beside it
(`apps/engine_audio_callback.h:34-40`) — while `hostReady` and `active` are RAW POINTERS to live
runtime atomics. **Readiness is read live; the mapping is read from a snapshot.** `completedBlockId`
belongs with the MAPPING and not with readiness: `apps/engine_consumer.cpp:655` copies
`shmView->completedBlockId`, a field of `SharedMemoryView` (`apps/host_controller.h:43`), while only
`:656` and `:657` take the address of a runtime member. An earlier draft grouped it with the live
pointers; codex-worker-1 corrected it, and the correction SHARPENS the hazard — a stale `TrackInfo`
reads the old mapping's completion counter, so its mapping and its progress agree with each other
and only readiness comes from anywhere else. Relaunch makes a NEW mapping (`apps/host_controller.cpp:286`, `:294`), and
`apps/engine_restart_worker.cpp` republishes readiness without republishing the callback's track
list. So a callback still holding the pre-relaunch `TrackInfo` can observe `hostReady == true`
against the OLD `shmView`. Whether anything republishes that list before the callback next runs is
NOT established here, and establishing it is what item 37 now asks for. **Retracting a wrong
mechanism is not the same as proving the right one safe**, and I conflated the two.

**What survives is PASS 7's rationale, which the packet never stated.** PASS 7 reads as a placement
preference; R16's analysis is the reason it is a SAFETY requirement. Producer-vector-only eviction
removes exactly the back-pressure invariant that makes a slot address unambiguous, while
`hostReady = false` additionally gates the input write and the mix, so the slot cannot be lapped for
that track at all. **The bullet was already right; what it lacked was the consequence of violating
it.** That is recorded here instead of as a defect. **ITEM 37's ORIGINAL THEORY is withdrawn — the
producer-vector reading — but the item itself is LIVE, PRODUCT and BLOCKING on a different and
narrower gap that this retraction does not touch: readiness and mapping are not published together.
An earlier draft of this sentence said the item was withdrawn outright while the item was already
re-opened, and the two contradicted each other in the same SHA.**

**How this got missed.** I read "eviction" as the word means in general — dropping a member from a
set — and derived consequences from that, inside a gate that defines the term four paragraphs above
the register sentence I was answering. **A gate's vocabulary is not the ambient one**, and PASS 7
exists precisely to forbid my reading. **WHAT FOLLOWS IS RETAINED HISTORY, NOT A CURRENT CLAIM** — it is the retracted argument, kept
verbatim because it is PASS 7's rationale, and every sentence in it should be read as describing the
producer-vector eviction that PASS 7 forbids rather than the shipped design:  The register directs the reviewer to confirm that evicting
a host which still owes dispatched blocks cannot corrupt what a RESUMED host reads or publishes. It
cannot be confirmed, and the opposite is derivable at the pin. The demand should be re-read as a
design requirement rather than a check.

Audio slots are addressed `blockId % numBlocks` on every side — engine
(`apps/engine_produce_block.cpp:724`), host (`apps/juce_host_process_main.cpp:635`), master
(`apps/engine_master_render.cpp:58`) and the audio callback (`apps/engine_audio_callback.h:363`).
Nothing stamps a slot with the block it holds; `apps/engine_audio_callback.h:362` states the mapping
as a COMMENT and reads on that basis. **The invariant that makes `blockId % numBlocks` unambiguous is
exactly `inFlight < numBlocks`** — the back-pressure gate at `apps/engine_producer_thread.cpp:282-283`.
Evicting a host from the minimum while it still owes dispatched blocks is, by construction, the
removal of that invariant for that host.

Three consequences follow, each a read of the pinned tree rather than a prediction:

1. **Corrupt read.** The resumed host binds input pointers from its OLD request —
   `blockIndex = request.blockId % numBlocks` (`:635`), then `audioInChannelPtr(…)` (`:643-648`) —
   with no test that the request is still current. The producer has since written a newer block's
   input into that slot.
2. **Corrupt publish.** It writes its output into the same lapped slot, and
   `apps/engine_audio_callback.h:363` reads that slot for the newer block with no identity check, so
   mismatched audio is PLAYED rather than dropped.
3. **The deadlock returns.** It stores its stale id — `completedBlockId.store(request.blockId)`
   (`:1085`), unconditional — and the producer reads it at `apps/engine_producer_thread.cpp:239` with
   NO monotonic clamp. `completedMinimum` then counts a host whose completed id has gone BACKWARDS
   and the gate closes again. **The eviction designed to open the gate re-closes it through the
   resumed host.**

`completedSampleTime` is not an escape. The host publishes it at `:1083` and its only consumer under
`apps/` is a test main (`apps/device_chain_ui_live_tests_main.cpp:170`); no production code reads it.

**What this means for G3.** Eviction cannot land as a producer-side rule alone — it needs a RESUME
protocol, and the packet must say so before any implementation is attempted. The requirement is
statable without designing it: a resumed host must be prevented from writing a lapped slot, and a
stale `completedBlockId` must not be able to lower the minimum. Whether that is a currency test the
host performs, a monotonic clamp on the reader, a re-baseline on resume, or a block-id stamp per
slot is an OWNER choice and this ruling deliberately does not make it. What is ruled out is the
option the gate currently accepts by silence: no protocol at all.

**R17 — item 21 (G3): THE CONTRADICTION DOES NOT EXIST, AND WHAT MANUFACTURED IT WAS READING A
PINNED PROVENANCE FIGURE AS A FORWARD CONSTRAINT.** The register asks the reviewer to resolve a
conflict between the check that places eviction lexically inside
`apps/engine_producer_thread.cpp:225-252` and "a sibling check" that pins the producer-exit count at
12, saying one of the two must yield. Neither has to.

**There is no sibling check.** G3's Static-checks paragraph names four: lexical containment in the
collection loop, no `chrono`/`steady_clock`/`system_clock`/`time(` token in the eviction span, a
containment event emitted on a TRANSITION and appearing exactly once under `apps/`, and the
`hostReady` write-form ratchet. None of them mentions the exit count.

**The 12 is a census row, and a census row is a fact about a FROZEN tree.**
`sed -n '134,373p' apps/engine_producer_thread.cpp | grep -c 'continue;'` returns 12, and A.0 checks
every such row for equality against the pinned product SHA — the `COMMAND-MISMATCH` comparison in
`tools/p12_selfcheck.py`, cited by TAG because an earlier draft cited it by LINE and the line moved
under it when the script grew, landing on `COMMAND-NEEDS-RG`. It
asserts what that command returns at that SHA and nothing else. An implementation lands on a
different SHA and is measured by that SHA's successor packet. **A pinned figure cannot forbid a
future edit — being unable to move is what pinning means.**

**And the packet already declares this number an inequality**: "The producer-exit count is a floor of
12: a `return` or a `break` out of a nested scope is not a `continue`." A floor says 12 UNDERCOUNTS
the real exits. **The defensible conclusion is only that 12 is not a forward acceptance
constraint** — an earlier draft of this ruling said eviction "adds a `continue` and takes the count
to 13", which overreaches: an implementation inside `:225-252` need not add one at all (it could
store `hostReady = false` and append inactive progress, or guard the existing append), and if it
does, a floor permits the increase anyway. codex-worker-1 named the overclaim. Either way nothing
here binds the implementation. So both readings of "sibling check" fail, in opposite
directions: as an acceptance check it does not exist, and as a census row it is pinned to a tree the
implementation will not be on, carrying an explicit floor besides.

**What is worth keeping is how the false conflict was manufactured.** A number stated with a
reproducing command reads as a rule, because that is what a reproducing command usually accompanies.
This packet's convention is the opposite — every census figure is a fact about `PRODUCT_SHA` — and
the register wrote a sentence that spends one as a budget. **A pinned measurement and a forward
constraint are indistinguishable in prose**; they are told apart only by asking which tree the number
describes, and that question is not visible in the sentence that uses it. The whole content of
item 21 (G3) was a packet decision, with nothing left for an implementer to choose between —
which is why this ruling discharges it, and item 21 records what that made it.

**What these rulings do NOT do.** None of them closes an item that names PRODUCT work — R1 through
R17 make such items implementable, R13 and R16 are RETRACTED rather than applied and R15's central claim is retracted within it, and each stays open until the work exists and is verified by
someone other than me. **A ruling CAN close an item whose whole content was a packet decision**, and
item 11 is the case: it asked for an authored population, R1 authored one, and there was nothing
left in it. That distinction was missing and the rule read as universal, which contradicted item
11's own CLOSED marker three SHAs running. That range — "R1 through R17" above — is checked against the rulings actually parsed
(`RULING-SET`), because it read "R1 through R4" for as long as there were eleven: the sentence was
written when four was the whole set and no later ruling was an edit to it. The manifest's parser
had the matching defect from the other side — `R[1-9]` could not match `**R10 — `, so R9's block ran
to the end and swallowed R10 and R11 into R9's text. **A hardcoded range and a summary sentence are
the same failure in two notations, and this packet has now shipped both.** A ruling recorded as a closure would be the same error as a
census recorded as a proof, which this packet has already made once at item 7.

# Open items — 39 atomic, 9 CLOSED at this SHA, 30 open

One per line, numbered in document order, so the count is checkable. ELEVEN are BLOCKING — 18, 19, 24, 26, 27, 28, 29, 33, 35, 36 and 37. **Items 26 and 27 became blocking when their populations were withdrawn rather than replaced, and BOTH have since moved on**: item 27's scope is authored under R12 and its blocker is the class-wide missing sender identity; item 26's population is complete across both planes and its blocker is the missing adjacent multi-plugin fixture. The withdrawal reasoning below is retained because it explains why a withdrawn population is a stronger blocker than a wrong one — a wrong one at least fails visibly — but neither item rests on it now.

1. **G0-B** — The generated header breaks the documented `-DDAW_BUILD_PATCHER_RUST=OFF` build for six unconditional targets, with no stated path, include directory or target-ordering edge.
2. **G0-B** — The declaring macro invalidates the `#[repr(C` grep this gate's population floor depends on.
3. **G0-B** — The mutation floor ">= 600" is asserted with no derivation and counts near 551 over the real 131 members, so the battery would fail its own floor permanently.
4. **G0-B** — 600 is a fifth pinned integer with no owner; the choice is carried in G0-B's review
   register, where owner choices are marked.
5. **G0-B** — CLOSED at this SHA. Both member counts are produced by a printed command over the pinned tree, the C++ one spanning the three headers the eight types are actually declared in. The per-type breakdown puts the entire 66-vs-65 difference in `EventEntry` (7 vs 6), which corroborates the one-sided-member claim rather than restating it.
6. **G1-A** — CLOSED at this SHA. The entry-address extraction's rule ships inside the printed command: the data-statement and index-site pipelines each carry their own exclusions and return 4 (3 production) and 12. Closing it is also what exposed the classification error the arithmetic had been hiding — the rule as prose said "four non-data operations" where the ready-flag operations are five, so the in-scope figure was 5 and is 4.
7. **G1-A** — **OPEN. REOPENED at this SHA after being wrongly closed.** The census I closed it
    with was lexical, and producer identity is not a lexical property: it is a property of
    EXECUTION AGENTS. Sixteen call sites are not sixteen producers, and one `ringWrite` site is not
    one producer — `git grep -n 'std::thread\|jthread' -- apps | grep -v _tests_main` returns 28,
    and any number of those threads may enter the single write site at `engine_ui_publish.h:110`.
    A single-WRITER claim needs ownership evidence: which thread may call `sendUiDiff`, enforced by
    something, not observed by grep.
    The consumer half was worse, and wrong even lexically. `git grep -n 'ringPop(ringUiOut' -- apps` returns 1, and that 1 is
    the whole problem: a second site passes the same ring under a RENAMED PARAMETER —
    `device_chain_ui_live_tests_main.cpp:57` calls `daw::ringPop(ringOut, entry)`. And the Rust side
    has a second READER I did not look for: `peek_ui_diffs`
    (`control.rs:444`) with **8** call sites in `ui/daw-cli/src/main.rs` — I first wrote 7, and 8 is
    what the frozen product has. It is a READER, not a second consumer: it walks `read` to `write`
    and never advances `read_index` (`control.rs:448-452`), so it does not compete for entries with
    the drain. That distinction matters and I collapsed it — a non-consuming reader breaks a
    single-READER claim and leaves a single-CONSUMER claim standing, and the item needs both. My claim of "one drain function, one caller, no second
    consumer in either language" was false in both languages. Multiple sidecar processes or
    `EngineHandle` instances can also drain concurrently, which no source census can see at all.
    **What a real closure needs**, recorded so the next attempt does not repeat this one: a
    singleton or ownership argument naming the thread that may write and the process that may
    drain, enforced by an assertion in the product rather than counted in the text. Until then the
    disarm rests on an unproven single-consumer contract, which is what the item said in the first
    place.

8. **G1-B** — CLOSED at this SHA, by withdrawal rather than by reconciliation. The three
   inconsistent statements of the send-site count all quantified over the hand-selected six, which
   this packet withdraws; the census is withdrawn with it rather than being made self-consistent,
   because a count over a withdrawn population re-admits the population under a different name. PASS
   7 is re-quantified over the two sites it names. Both return under item 11 (G1-B).
9. **G1-B** — CLOSED at this SHA as a defect, SUBSUMED by item 11 (G1-B) as subject matter.
   The contradiction was between a scope that omitted a region and a population that called that
   region one of "the only two" rewriting in place. The population making that claim is withdrawn,
   so the contradiction has no second side; the phrase appears nowhere in this packet. It returns
   the moment a derivable population does, and it returns as a REQUIREMENT on that population — any
   successor's population must be checked against the scope before it is published, which is the
   check whose absence produced this item.
10. **G1-B** — CLOSED at this SHA as a defect, SUBSUMED by item 11 (G1-B). Both recipes now
    state what they actually return rather than a list they do not produce: `pub fn read_` returns
    21 candidates and is explicitly NOT a population, and the `Region*` intersection returns 2 and
    is published as WRONG with the FAIL clause it shipped with. A recipe that names its true output
    cannot fail to reproduce its list, because it no longer claims one.
11. **G1-B** — **CLOSED at this SHA by AUTHORING under R1: SEVEN.** The request set is the
    `Request*` UiCommandType enum values (7) and the population is their ANSWERS, not filtered by
    transport: six region readers plus `drain_ui_out` for `RequestChainSnapshot`, whose answer
    carries no request identity and has 0..N cardinality — the sharpest instance of the gate's own
    subject, which a region-based rule had excluded. Previously: The `requestSeq` predicate is
    CIRCULAR: it cannot see a request/answer reader that lacks a correlation token, which is the
    defect the gate exists to find. `read_device_params` is request-driven with no `requestSeq`;
    `read_clip_window` carries a `requestId` under the global seqlock. Previously: A reader is
    request/answer iff the value carries `requestSeq`, equivalently iff guarded by a per-slot `seq`
    rather than the global `ui_version`. Two independent markers select the same 4, and the 21
    partition 4 / 10 / 7 with the third class named. The number SIX is recorded as UNREPRODUCED:
    two independent measurements returned it via two different boundary bugs, which is why the
    packet records the method and not only the members. The hand selection of six was irreproducible; the predicate proposed to replace it yields two, not six, and is withdrawn. This gate cannot be decided until a population exists, as G3 cannot until N exists.
12. **G2-A** — Layer-1 fixture arithmetic: eleven journal lines, not six, and ids legitimately repeat, so a correct implementation fails the gate's only runnable integration assertion.
13. **G2-A** — CLOSED at this SHA. The fifty-one correlator sites are no longer carried on attribution: the G2-A population states the command, its raw count and the subtraction, and reaches the exact review's 4 + 6 + 17 + 24. The count is written once, there, so this entry does not restate it. Re-deriving it is also what refuted my own competing figure — that one counted mentions rather than call sites, so the disagreement was my predicate and not the reviewer's arithmetic.
14. **G2-A** — The BATCH note branch: `resolve_base` keeps the counter crossing on the path a browser transpose takes, and the static check as written is satisfied by fixing the chord branch alone.
15. **G2-B** — The self-deadlock: the admitted fix class requires `applyHostBypassStates` to stop taking `controllerMutex`.
16. **G2-B** — The swap trap rests on an unratcheted guard at `apps/daw_engine_main.cpp:1107-1109`.
17. **G2-B** — Probe ordering: without forbidding the offline probe from acquiring before the RT probe reports, the PASS token is producible by the packet's own fixture.
18. **G2-B** — ⟦PRODUCT⟧ ⟦PACKET⟧ **BLOCKING, RULED (R2) and PROPAGATED into the gates at this SHA; open for
    BOTH product and packet work.** The circularity is dissolved in the specification: dispatch is
    permitted at `mapped-and-bypassed`, so the ack that establishes `mirror-complete` arrives during
    a `ProcessBlock` the gate now allows. What remains in PRODUCT is building the two levels. **What
    remains in this PACKET is that G2-B's PASS 4 is still withdrawn with no replacement, so no PASS
    condition accepts the two-level design R2 ruled** — an earlier version of this line said "open
    for the product implementation only", which the item's own kind marker then contradicted. **WHAT FOLLOWS IS RETAINED HISTORY, NOT A CURRENT
    CLAIM** — the original statement of the circularity, kept because R2's ruling is only legible
    against it, and superseded by the two paragraphs above. It was introduced as "Original statement
    of the circularity", which is a label rather than a boundary, so its "unspecified until" and
    "open for the propagation" read as live status to anything consuming the whole paragraph. The ack arrives only during a `ProcessBlock` that `processTrack` refuses while `hostReady` is false. A PASS bullet was withdrawn rather than reworded, and the owner HAS ruled (R2: two-level readiness) and the mirror half is unspecified until that ruling is PROPAGATED into this gate and G4 — the item is open for the propagation, not for the decision.
19. **G3** — ⟦PRODUCT⟧ **BLOCKING, not closed. The ruling is MADE (R3: N = 3, authored) and no further ruling is required.** N has no source in the tree; R3 supplies it as an authored constant. What remains is implementation, not decision (R3: N = 3, authored), which makes the gate decidable for PLANNING; it is not decidable for ACCEPTANCE until the ticket lands — N pinned with units and semantics, a static check on the literal, watchdog instrumentation, drift acceptance and independent validation.
20. **G3** — `DAW_ENGINE_DEBUG_STALL` must be set by the Layer-2 fixture or the channel a PASS bullet reads does not exist.
21. **G3** — **CLOSED at this SHA.** The static-check contradiction was not one. R17 shows there is no sibling check to conflict with — G3's four static checks never mention the exit count — and that the 12 is a census row pinned to `PRODUCT_SHA`, checked for equality against a frozen tree the implementation will not be on, and already declared a FLOOR ("a `return` or a `break` out of a nested scope is not a `continue`"). An implementation inside `:225-252` need not add a `continue` at all, and if it does, a floor permits the increase — either way nothing here binds it. Nothing for an implementer to choose between; a packet decision with no product work in it.
22. **G4** — The fixture definitions added here (F0-F6, S1-S8) and the corrected ack-census counts have not been run against anything; the reviewer should confirm them against the fixture rather than against this packet.
23. **all** — **CLOSED at this SHA.** `read_clip_window` IS a request/answer reader:
    `RequestClipWindow` is command 30 and it reads that command's region. The exact review was right;
    it was invisible to two earlier predicates because it carries no per-slot token and is guarded by
    the global `ui_version`. Originally: It cannot be placed until item 11 gives the gate a population.

24. **G0-B** — ⟦PRODUCT⟧ **BLOCKING for G0-B only.** **RULED (R4): the Rust side renames.** The pure name join requires one rename: `patcher_abi.h:75`
    declares `uint8_t reserved[4]{}` where `patcher_rust/src/lib.rs:86` declares `pub _pad0: [u8; 4]`,
    so the two sides of a byte-identical member disagree by NAME and no rule that forbids an alias
    table can join them. PASS 9 is RED until this lands and states so. **RULED (R4): the RUST
    side renames**, `_pad0` becomes `reserved`. The convention argument — `_pad0` is idiomatic Rust —
    was weighed and lost: a binding may be unidiomatic, an authority may not be derived from its
    binding. This item is open for the RENAME, not for the choice, which is made.
25. **all** — **RULED (R1): authored with drift detectors.** The SIX HAND-CLASSIFIED populations
    — five semantic groupings plus G1-A's authored cross-language RING index population, which
    became the sixth when the R1 mechanism was applied to it, tracked rather than claimed away. `Regions the engine addresses` (8), `Bounds checks anchored on the child's number` (7), `Ring constructions over a host-created mapping` (3), `One-sided members` (1) and `Tracks whose production must continue` are semantic groupings, not text matches: no grep distinguishes a REGION from the
    offset fields that address it, and counting `Offset` declarations in `apps/shared_memory.h`
    returns 27 against a claim of 8, which is the distance between the two ideas. Each carries the
    marker and this item; a sixth appearing without one fails the gate. Deriving them needs a
    predicate nobody has proposed, so this is open, not closed.

26. **G4** — ⟦PRODUCT⟧ ⟦PACKET⟧ **BLOCKING on ONE missing fixture and its oracle: an adjacent
    multi-plugin segment.** The input relation is enumerated — seven derived census rows, every
    member pinned by path, line and content — so the incompleteness that reopened this item is
    closed. **THE BROAD REASON THIS ITEM USED TO GIVE IS FALSE AND IS WITHDRAWN**: it said G4's PASS
    conditions, deterministic test and static checks were "all written against the OUTPUT relation",
    which the next sentence of this same item then contradicts — G4 specifies input coverage in S2,
    S5, PASS 2's ordering and PASS 4's input immutability. An item may not publish a false broad
    account and its true narrow replacement as though both were current. **Both specifics I gave for that were wrong and are withdrawn.** I wrote that failure model (1)
    has "no test at all" — G4 specifies input coverage in S2, S5, PASS 2's ordering and PASS 4's
    input immutability, so the gap is not the absence of a test. **The precise missing case is a
    MULTI-PLUGIN SEGMENT fixture**, and it is the only one that can exercise the alias rebinding at
    `:1061`. I also wrote that the rebinding NARROWS the host's ownership of the input plane to one
    plugin. It does not: the invariant runs from dispatch to the segment's acknowledgement, there is
    no per-plugin signal for the engine to observe, and what the host happens to READ after plugin
    one cannot shorten a window the engine has no way to learn about. **A fact about behaviour is
    not a licence to narrow a contract** — that is the same move as reading a race as absent because
    the code usually wins it.
    **The incompleteness that reopened this item was the third in this population and the first that
    was not a selector defect.** The four-spelling census missed a spelling; the mapped-base census
    missed helper-mediated readers; that one missed HALF THE SUBJECT, because I enumerated what the
    discussion was about instead of what the invariant says. No better selector would have caught
    it — only reading the sentence the gate is built on. Previously: the population is measured from
    the MAPPED BASE with aliases
    followed and roles assigned by USE at the dereference, sized by the two OUT census rows and
    split direct/indirect per R8(c), with two same-agent readbacks excluded by argument and the
    establishing and non-site sets accounted so the arithmetic closes. Those figures are CITED, not
    restated here: a count written twice is the surface codex-worker-1 mutated, and this paragraph is
    history, which is the worst place for a live number to hide. The
    four-spelling selector is RETIRED with its blindness quantified: it saw 6 of 7 readers and none
    of the aux writes. Previously: the out-plane population was the byte-consuming reads at
    `engine_produce_block.cpp:923`, `:1150` and `engine_master_render.cpp:100`,
    separated from plane-establishing sites and byte-producing writes by ROLE rather than by name.
    The `:670`/`:730` pair that forced the withdrawal is resolved by putting both in the same role:
    they establish a plane and consume nothing, so no rule has to prefer one. Drift detector on the
    out-plane command's raw figure, which is stated once in the population and not restated here.

27. **G2-A** — ⟦PRODUCT⟧ **BLOCKING, and the scope is AUTHORED under R12: the gate ranges over the three
    production adoptable-refusal emit sites.** The retraction is lifted. Every earlier scope here was
    falsified by the same move — it selected on the ARBITRATION and the gate is about the ANSWER, so
    it kept including commands whose refusal is TERMINAL AND UNADDRESSED — the ResyncNeeded
    channels, which DO carry a retry value and carry no command field — and excluding `SetRowOps`,
    which emits an addressed refusal while being arbitrated by nothing. There are THREE refusal channels
    (adoptable `ClipRejected`, `UiDiffType::ResyncNeeded`, `UiHarmonyDiffType::ResyncNeeded`) and
    treating them as one is what made every containment claim false. **This item stays BLOCKING for the
    PRODUCT defect, not for its scope: ALL THREE members cannot satisfy the invariant** —
    `UiClipRejectPayload` carries no sender-minted per-command identity, so `commandType` names a
    KIND and not an instance for every member, and `engine_rowops_commands.cpp:49` is additionally
    worse. **That class-wide defect is THIS item's**, not item 29's — item 29 is only the narrow
    `SetRowOps` zero-base case, and pointing 27 at it understated the work by two emit sites.
    codex-worker-2 ruled the attribution. This paragraph also said "one of the three" for two SHAs
    after R12 said all three: I corrected the RULING and left the ITEM. Previously: The scope is two populations related
    by containment: A (arbitrated against a version counter, 9 call sites) and B (emits an adoptable
    `ClipRejected`), with **B = A ∪ {SetRowOps}** determined from the three production emit sites
    rather than from a name list, and a drift detector on that site count. Every control must name
    which it ranges over. What is NOT closed is the PRODUCT defect this exposed, which is open item
    29 (G2-A). **The two populations are NOT NESTED**, which is the finding that
    settles the shape of the fix: "commands arbitrated against a version counter" and "commands that
    emit an adoptable refusal" are different sets, and `SetRowOps` is in the second and not the
    first. Any control here must say WHICH population it ranges over.
    **Measured at the frozen product by claude-worker-1, and it is worse than "outside the nine":**
    `UiSetRowOpsPayload` has NO `baseVersion` field, so `SetRowOps` cannot be arbitrated even in
    principle, yet `engine_rowops_commands.cpp:49` emits an adoptable `ClipRejected` with
    `/*sentBase=*/0, /*currentBase=*/0` — constants, where the other TWO emit sites pass real
    values and the payload's own comment calls `currentBase` "the value to retry with". There is
    nothing to retry with. **And zero is a live value**: `clipVersion` initialises to 0
    (`shared_memory.h:1376`, `daw_engine_main.cpp:675`), so a consumer cannot tell these zeros from a
    genuine base of 0 on a fresh track — the one value that should mean "nothing to say" already
    means something on this wire. **The consequence is LATENT, not wired, and I defended the wrong half of this.**
    `ui/daw-cli/src/main.rs:1179` correlates on `(track, command_type, sentBase)` and
    `await_clip_outcome` is live with four callers (`:2989`, `:3000`, `:5418`, `:5430`) — so when
    codex-worker-1 said the correlator was dead I checked, and it is not. But **none of those
    callers is SetRowOps**: the CLI's SetRowOps path at `:4263-4288` sends and prints, with no await
    at all, and the agent and sidecar do not wait either. So the inert third key harms no consumer
    TODAY; it makes the refusal uncorrelatable for any consumer that starts waiting. I was right
    that the function is live and wrong about the case this item uses it for, which is the half that
    mattered. Acceptance is unaffected — `applySetRowOps` bumps the clip
    version at `engine_clip_edit.cpp:439` — so this is a refusal-path defect only. The arbitrated-command SCOPE is FALSIFIED and unreplaced, so G2-A
    governs an undefined set. Falsified by exhibit: `engine_rowops_commands.cpp:49`
    emits an adoptable refusal for `SetRowOps`, outside the nine. The 51 correlators also span
    chain, sampler, mod-link and journal refusals. Either the nine becomes ten-or-more by
    enumeration, or the gate states which refusals it governs and why the rest are out — and the two
    populations must stop being treated as one set.

28. **G2-A** — ⟦PRODUCT⟧ ⟦PACKET⟧ **BLOCKING, and it was NONBLOCKING for too long.** G2-A's PASS 4
    requires every counter-only decision to be replaced, and `ui/daw-cli/src/main.rs:1183-1184`
    returns `ClipOutcome::Applied` solely because the track's clip version moved — no identity of
    the command that moved it is read. Items 27 and 29 do not touch that consumer, so nothing else
    on the blocker list closes it. It needs PACKET authoring (the extraction predicate, its command
    and its member list, which this item has never carried) and PRODUCT replacement of each such
    decision. codex-worker-1 supplied the argument and the call site; I marked it after reading
    `main.rs:1183-1184` at the pin rather than on the claim. The requirement that was S3, split out
    of item 27 because they are separately
    fixable and bundling them means neither can be closed: the counter-only decisions must be
    extracted and each replaced, and no check over that extraction can be written until the
    extraction has a predicate, a command and a member list.

29. **G2-A** — ⟦PRODUCT⟧ **BLOCKING. The `SetRowOps` refusal is malformed for the channel it rides**, measured
    at the frozen product by claude-worker-1 and independent of this packet's scope question. It was
    unmarked while item 27 delegated its blocking status to it — **an item can only delegate to an
    item that carries the weight**, and for two SHAs the delegation pointed at an item the manifest
    reported as non-blocking.
    `engine_rowops_commands.cpp:49` passes `/*sentBase=*/0, /*currentBase=*/0` where the other
    emit sites pass real values, and the payload's own comment calls `currentBase` "the value to
    retry with" — there is nothing to retry with. Zero is a LIVE value (`clipVersion` initialises
    to 0 at `shared_memory.h:1376` and `daw_engine_main.cpp:675`), so a consumer cannot distinguish
    these from a genuine base of 0 on a fresh track. `ui/daw-cli/src/main.rs:1179` correlates on
    `(track, command_type, sentBase)`, so for `SetRowOps` the third key is INERT and two concurrent
    refusals on one track would be indistinguishable — **but NO CONSUMER AWAITS A SetRowOps REFUSAL
    at this SHA**, so the collision is latent rather than live. The CLI's SetRowOps path
    (`ui/daw-cli/src/main.rs:4263-4288`) sends and prints; `await_clip_outcome`'s four callers pass
    note, delete and chord command types. This item said the collision was happening and item 27
    said it was not, for a SHA and a half — **two items of one gate holding opposite readings of one
    line of Rust**, because I corrected the sentence where the argument lived and not the sentence
    where the finding was filed. Acceptance is unaffected: `applySetRowOps` bumps the clip version at
    `engine_clip_edit.cpp:439`. This is a refusal-path defect and it is a PRODUCT fix, not a packet
    edit.

30. **G2-A** — **RULED (R10): the wiring is DEAD, the hazard is REAL, and they are separate work.**
    Remove the unused `requireMatchingClipVersion` member from `UndoCommandDeps`; separately, undo
    replaces the whole document via `applyDocument`, so an edit made between a user's view and their
    undo is silently reverted, and covering that needs a DOCUMENT-level version which does not
    exist. Deleting the wiring must not read as dismissing the risk. Originally: the undo path holds
    an arbiter it never calls, measured by claude-worker-1 at
    the frozen product with TYPE-LEVEL evidence rather than a call-site census, which is the only
    kind that can see it: `engine_undo_commands.h:38` declares the clip arbiter as a
    `std::function` member and `engine_undo_commands.cpp` contains ZERO occurrences of the name.
    Undo and Redo mutate clips and are wired to receive the per-track optimistic-concurrency gate
    without consulting it. **The absence IS the finding**, so no census over call sites can produce
    it. **That tail is SUPERSEDED TWICE and is kept as history.** It said the dead-wiring question was an
    owner call I had not made and that item 27's scope could not close until it was — R10 made the
    call (dead wiring, removed separately from the hazard) and R12 authored item 27's scope without
    reference to the arbitrated set at all, because the gate ranges over the EMIT sites. So the
    sentence describing what blocks item 27 outlived both the ruling that answered it and the ruling
    that made it irrelevant. **A dependency between items is a claim like any other and goes stale
    like any other**, and this one survived two rulings because nothing re-reads a sentence that
    merely says what is blocked on what.

31. **all** — **The pointer-helper RATCHET, and it must pin the GENERAL population, not G4's
    subset.** The completeness argument rests on a closed set of functions returning a plane
    pointer. A ratchet pinned at "two OUT-PLANE helpers" would have to decide which plane a NEW
    helper serves, and the only thing available to decide that is its NAME — the selector defect
    reappearing inside the guard built against it; a `safeAudioAuxPtr` would be classified by
    spelling. **Pin instead: every `float*`-returning function or lambda in non-test `apps/*.cpp` that
    reaches a segment base.** The ratchet holds at **four** at this SHA (`audio_shm.cpp:5`, `:17`,
    `engine_produce_block.cpp:848`, `:861`) and is now EXECUTED rather than described
    (`RATCHET-DRIFT`): A.0 runs the predicate against the pinned tree, compares the sites AND the
    fingerprint of each line, and compares the count against the word in this sentence. It was prose
    in every SHA before this one — a ratchet that is only written down is a ratchet that has never
    been pulled. The `.h` declarations are excluded because a declaration returns no pointer; the
    predicate is over definitions. That predicate needs no judgement about planes, so a
    fifth of ANY name or plane turns it red and a person decides. **The rename-invariance claim is WITHDRAWN and the reason is worth keeping.** This item said:
    "add a fifth and it must fire; RENAME an existing one and it must NOT, because the count is over
    structure." The implemented ratchet pins each member as (path, line, fingerprint of that LINE),
    and a line contains the function's name — so a rename DOES fire, and the stated invariant is
    false about the check that enforces it. **The count is over structure; the FINGERPRINT is not**,
    and I wrote the invariant before the mechanism existed and never re-read it against what got
    built. A rename firing is arguably the safer failure — a renamed helper deserves a look — but it
    is not what the packet promised, and a guard whose documented behaviour differs from its actual
    behaviour is the thing this packet exists to prevent. What survives: add a fifth of any name or
    plane and it fires, which is the property the completeness argument actually needs.
    **It belongs with the LayoutSpec work in constraint 1, not inside either gate**, because G1-A's
    input-plane writers and G4's out-plane readers share this ratchet without sharing a population.

32. **G2-A** — **RULED (R11).** `commandMutatesDocument(Undo)` returns FALSE with the comment "no
    document state" (`engine_command_mutates.h:58`) while `handleUndo` calls `applyDocument` and
    replaces the entire `ProjectDocument`. The classifier answers "does this command CARRY document
    state in its payload"; its NAME asks "does it MUTATE the document"; and G2-A's arbitrated
    population is derived from it. A predicate this gate depends on is inconsistent with the
    behaviour it names, which is a defect in the predicate and not in its wording.

33. **G1-B** — ⟦PRODUCT⟧ **BLOCKING. The tests PASS 9 and S4 name do not exist.** Restoring them from
    "withdrawn with the population" to live conditions made the gap visible: PASS 9 requires a
    mechanical echo ratchet over the SEND SITES — a population nobody has enumerated, so that bullet
    is blocked on a missing population and not only on an unwritten test — and S4 requires drift
    detectors pinning the authored population's size. **NEITHER RUNNABLE ASSERTION IS IMPLEMENTED** —
    G1-B states the detectors' commands and figures, and nothing executes them; this line said
    "neither is written", which literally denies the specified detectors as well. This item repeated "the
    seven authored send sites" when it was filed, taking a count from the request/answer population
    and a unit from a population that does not exist; PASS 9 states it correctly and this item now
    matches it. While they read "withdrawn", G1-B's
    acceptance flag was true and nothing contradicted it, because a withdrawn condition is not a
    failing one — **the withdrawal was carrying the gate's acceptance**. This is the same shape as
    item 26 (G4): a gate whose population is authored and whose tests are absent, and in both cases
    the population work is what got done because it is the part a packet can do. Product work; the
    detectors belong with the LayoutSpec assertions in constraint 1, and the ratchet is a sibling of
    the one item 31 (all) now executes.

34. **G2-B** — **WITHDRAWN at this SHA. Filed on a false premise and retracted the same day.** It
    claimed `mirrorOnly` was a consumption boundary keyed on PRIMED where the invariant needs ACKED.
    `mirrorOnly` is read at two sites only — `engine_produce_block.cpp:602` and `:620`, both
    guarding renderTrack and note emission — and `sendProcessBlock` is outside both, so it is not a
    consumption boundary; and `writeMirrorParams` stamps mirrored params into the same time-ordered
    ring as the block, so they drain before its DSP. See R13, which records the retraction and the
    reasoning error rather than deleting them. **Kept in the list rather than removed**: an item
    that was blocking for a day and then vanished would read as work completed.

35. **G0-B** — ⟦PRODUCT⟧ **BLOCKING. The patcher's `EventEntry` has no `ready` field and its writer stores the
    whole object over C++'s.** `patcher_rust/src/lib.rs:97-105` is `#[repr(C, align(64))]` with six
    members summing to 60 bytes, so [60,64) is TAIL PADDING; `apps/shared_memory.h:450` puts the
    multi-producer publication flag `ready` in exactly those bytes; and `push_event`
    (`patcher_rust/src/lib.rs:158`) writes `*slot = entry` for the whole 64-byte object. Rust does
    not guarantee that padding bytes are written and does not guarantee they are not — a
    `size_of`-wide copy may include them — so this is UNPROVEN SAFE rather than proven broken, which
    is precisely the state a gate must stay blocked on. **THE CONSEQUENCE I GAVE WAS FALSE AND IS WITHDRAWN.** I wrote that a
    patcher-emitted event clears a slot's `ready` so the engine refuses a fully-written command. It
    cannot: `ctx.event_buffer` points at `buffer.events` (`apps/engine_run_patcher_node.cpp:82`),
    which is `std::array<daw::EventEntry, kPatcherNodeCapacity>` (`apps/engine_types.h:86`) —
    ENGINE-LOCAL memory consumed by count, not the SHM command ring — and the later ringStd path
    stages a copy and forces `ready = 0` before publishing. codex-worker-1 caught it. **What remains
    is an ABI CONFORMANCE defect rather than a runtime hazard:** G0-B's population declares these
    two structs one ABI and they disagree member-for-member. `sizeof` agrees only because the Rust
    side's missing field is covered by padding, so every layout assertion over the pair passes for a
    reason that is not layout equality — and the first use of that struct against shared memory, or
    the first member added to either side, breaks silently. That is what the gate stays blocked on. **R14 chooses between the two fixes that were open here, because leaving them open was itself the
    defect codex-worker-1 named:** G0-B's invariant demands member parity while PASS 8 permits a
    one-sided member on byte-disjointness, and this pair cannot satisfy both. Parity wins; a
    field-wise copy alone does NOT close this item, since it would leave seven members against six.
    Give the
    Rust struct an explicit `ready: u32` so the layouts match member-for-member. **An earlier version
    of this sentence offered "or make `push_event` assign field-wise" as an alternative — the fix R14
    explicitly rejects two paragraphs above, because it would leave seven members against six and
    the invariant unmet.** An item may not offer a remedy its own ruling refuses. PRODUCT work.
    **How this got missed:** the register asked the right question and I answered it by reading
    `ui/daw-bridge/src/layout.rs`, a DIFFERENT Rust endpoint that does declare `ready`, then marked
    it resolved. See the G0-B register entry, which now records the substitution.
36. **G3** — ⟦PRODUCT⟧ ⟦PACKET⟧ **BLOCKING. `tools/host_stall_check.sh` CANNOT DISTINGUISH THE
    OUTCOMES IT IS ASKED TO DECIDE, and no measurement that could distinguish them exists yet.** An earlier headline said
    the check contradicts the shipped rule so one of the two must be wrong — which this item's own
    body then conceded is false, since the failed-send drop the check's comment names is real
    (`apps/engine_produce_block.cpp:1096-1100`). The defect is not a contradiction; it is that the
    check counts log lines and therefore reads a gated producer, a dropped host and a run that never
    played as the same zero. The check's pass
    predicate (`:105-109`: at most three post-marker `producer stall (inFlight)` lines) PASSES only
    when few such lines are logged, which the check reads as a SIGSTOPped host having stopped holding
    the minimum — an inference, not an observation, and the two come apart below. Its comment names
    the mechanism it believes in: "the host is dropped" (`:101-104`). `completedMinimum` (`apps/engine_rt_helpers.cpp:492-511`) counts exactly
    such a host WHILE THE GATE HOLDS — its comment's "the host is dropped" is NOT a fiction, since a
    failed send drops one (`apps/engine_produce_block.cpp:1096-1100`,
    `apps/engine_consumer.cpp:840-841`); what the check cannot do is tell the two apart. Its three
    exclusions are `!active`, `completedBlockId == 0` and owes-nothing, and a
    frozen producing host meets none (and the `!active` exclusion is unreachable in production —
    `:249-251` hardcodes it). The gate at `apps/engine_producer_thread.cpp:282-283` then closes and
    the frozen host's completed id cannot advance on its own — its `lastDispatchedBlockId` still
    can, on a later dispatch (`apps/engine_produce_block.cpp:1072-1108`). R15 narrows the stronger claim this item used to
    carry: the state is NOT statically absorbing, because `:232-233` and `:236-237` skip a host
    on a failed try_lock or an absent mailbox, so the counted population varies between
    iterations. No LIVENESS drop exists, but a
    failed-send drop does (`apps/engine_produce_block.cpp:1096-1100`), so the check's comment is not
    a fiction and R15's mechanism ruling against it is RETRACTED. What makes this
    PRODUCT work is that the check has at least two identified ways to report zero stalls while the
    producer's gate is never observed, both found by codex-worker-1 and both decidable at the pin: its
    `froze-host-marker` is appended through a second file description and can be overwritten by the
    engine's own next write (`:65` vs `:89`), and `if (isPlaying)` at
    `apps/engine_producer_thread.cpp:284` wraps only the `logStall` call while the `continue` at
    `:289` sits outside it, so a producer that never started playing — `cli do play … || true` at
    `:77` discards the failure — stays gated and silent. **A check counting log lines cannot see a
    gate that does not log.** Fixing the check means making its oracle the gate rather than the
    logging of the gate; fixing the comment at `:101-104` means describing the mechanism that DOES
    exist — a failed-send drop, not the unconditional drop it implies. Both are PRODUCT edits. M1 or an equivalent measurement IS owed — R15 gives the
    single status, and retracts the static settlement that had made it dispensable, because a real
    drop path exists at `apps/engine_produce_block.cpp:1096-1100` and nothing static separates
    "gated and stayed gated" from "gated, dispatched, send failed, host dropped". It cannot be built
    from today's telemetry: the per-host vector carries no host identity and no `lastDispatchedBlockId`, so
    nothing currently emitted can evaluate the owes-nothing predicate at
    `apps/engine_rt_helpers.cpp:502`. PRODUCT work.
37. **G3** — ⟦PRODUCT⟧ ⟦PACKET⟧ **BLOCKING, and the reason has MOVED: readiness and the mapping it authorises
    are not published together.** `EngineAudioCallback::TrackInfo` holds `shmView`/`shmBase`/`header`
    as a SNAPSHOT and only `hostReady`/`active` as raw pointers to live runtime atomics
    (`apps/engine_audio_callback.h:34-40`; `apps/engine_consumer.cpp:655-657`). `completedBlockId`
    travels WITH the mapping — it is a `SharedMemoryView` field (`apps/host_controller.h:43`) — so a
    stale `TrackInfo`'s mapping and progress counter agree with each other and readiness alone comes
    from elsewhere. Relaunch installs a new mapping
    (`apps/host_controller.cpp:286`, `:294`) and `apps/engine_restart_worker.cpp:87` publishes
    readiness without republishing the callback's track list, so a callback holding the pre-relaunch
    `TrackInfo` can see `hostReady == true` against the OLD `shmView`. G3's eviction rests on
    `hostReady = false` excluding a host at three points; two of them are snapshot-free, and the
    MIXER is not. Whether some other path republishes the list first is not established, which is
    the state a gate stays blocked on — the same standard item 35 is held to. What closes this is a
    generation binding: readiness and mapping published as one, or a mapping generation the callback
    can test. codex-worker-1 found this. **THE ACCEPTANCE ORACLE IS NOT WRITTEN AND THIS ITEM NAMES
    WHAT IT MUST COVER**, because G3's deterministic test, PASS conditions and static checks are
    unchanged and none of them can see this: acquire a generation-old `TrackInfo`, barrier before its
    live gate read, relaunch and publish, then prove every callback path REJECTS the old generation
    and accepts the new one, with a negative control. **The readers are the four `acquireTracks` consumers**, and an earlier draft
    named two of them while listing two PRODUCER surfaces as if they were readers — a list wrong in
    KIND as well as in count. They are `process()` (`apps/engine_audio_callback.h:232`),
    `awaitAnyReadyTrack` (`:772`), `awaitAllReadyTracks` (`:861`) and `awaitNextBlock` (`:912`). The
    two `await*ReadyTrack*` gates are production OFFLINE paths, called from
    `apps/engine_offline_render.cpp:123` and `:149`, and omitting them would have left the render
    path uncovered. The publication surface is `updateTracks`
    (`apps/engine_consumer.cpp:737`). Ordinary `TrackInfo` construction is `:650-674` with cache
    reuse and push at `:678-685`; `:692-735` COPIES those records to build the aux ones — an earlier
    draft said `:692-735` constructs both, which is wrong about the ordinary half.
    `restartTrackHost` (`apps/engine_track_setup.cpp:367-403`) and the restart worker mutate mapping
    and readiness; neither publishes a `TrackInfo` list. An earlier draft called all three
    "publication surfaces", blurring three roles into one; the oracle must exercise construction,
    publication and both relaunch paths distinctly, and none of them is a reader that must reject a
    stale generation. The supported `--no-spawn` path needs an explicit external-host policy besides:
    connect records no owned PID and relaunch kills only `pid > 0`
    (`apps/host_controller.cpp:197-202`, `apps/engine_track_setup.cpp:37-44`), so a stopped external
    host can survive a relaunch and still hold the old mapping — and the policy must cover the
    NAMESPACE race that follows: `launch` reuses the socket and SHM names, so a resumed old host's
    cleanup can unlink the NEW host's namespace (`apps/host_controller.cpp:156-159`, `:216-267`;
    `apps/juce_host_process_main.cpp:441-447`, `:1139-1150`). codex-worker-1 enumerated all of it.
    **THIS ITEM IS NOT PRODUCT-ONLY**: writing the oracle into G3's deterministic test, PASS
    conditions and static checks is PACKET work, and the opening's claim that every blocker needs
    product work is not WRONG about it — item 37 has product work — but it is
    incomplete: this item is not product-ONLY. **WHAT FOLLOWS IS RETAINED HISTORY, NOT A CURRENT
    CLAIM.** The item's ORIGINAL claim is withdrawn and every sentence after this one describes that
    theory rather than the live defect above — it read eviction
    as removal from the producer's minimum, which
    PASS 7 names as its own refutation, and the Invariant at `:961-965` requires `hostReady` stored
    false instead. At the pin that excludes the host from the producer loop
    (`apps/engine_producer_thread.cpp:226-227`), from any newer input or output write
    (`apps/engine_produce_block.cpp:362-363`) and from the mix
    (`apps/engine_audio_callback.h:305-306`), and readiness returns only via a RELAUNCH
    (`apps/engine_restart_worker.cpp:70`, `:87`), so SIGCONT cannot readmit the frozen host. None of
    the three consequences follows. codex-worker-1 refuted it. The analysis is kept in R16 as PASS
    7's rationale — the reason that bullet is a safety requirement and not a placement preference.
    What this item claimed: Every side addresses an audio slot as
    `blockId % numBlocks` — `apps/engine_produce_block.cpp:724`,
    `apps/juce_host_process_main.cpp:635`, `apps/engine_master_render.cpp:58`,
    `apps/engine_audio_callback.h:363` — and no slot carries the id of the block it holds, so the
    mapping is unambiguous only while `inFlight < numBlocks` holds. G3's eviction removes exactly
    that for the evicted host. A resumed host then binds its pointers from a stale
    `request.blockId` with no currency test (`:635`, `:643-648`), writes a lapped slot the audio
    callback reads without an identity check, and stores its old id unconditionally at `:1085`,
    which the producer consumes at `apps/engine_producer_thread.cpp:239` with no monotonic clamp —
    lowering the minimum and re-closing the gate the eviction existed to open.
    `completedSampleTime` cannot serve as the guard: its only consumer under `apps/` is a test main
    (`apps/device_chain_ui_live_tests_main.cpp:170`). **R16 rules that the register's
    confirmation cannot be given and states the requirement instead**: a resumed host must not write
    a lapped slot, and a stale `completedBlockId` must not lower the minimum. Which mechanism
    delivers that — a host-side currency test, a reader-side monotonic clamp, a re-baseline on
    resume, or a per-slot block stamp — is an owner choice R16 deliberately leaves open, marked
    in G3's review register. PRODUCT
    work.

38. **G1-B** — ⟦PACKET⟧ **NOT BLOCKING. THE EFFECT of a live non-gate prerequisite on planning is
    UNRULED, and at this SHA the manifest says so instead of guessing.** Scoped to G1-B because
    that is where the prerequisite is LIVE, though the derivation is shared. G1-B carries "the
    production atomic size/alignment assertions"; the five gates whose dependency closure reaches
    G1-B inherit it. **What was published before this SHA was worse than an open question**:
    `non_gate_prerequisites` was emitted while `decidable_for_planning`, `planning_blocked_by` and
    `plannable_with_dependencies` were derived from the withdrawn-population predicate and a
    closure following gate IDs only — so every gate read `plannable: true` and a planner walking
    the graph could not see the prerequisite at all. Restoring G3's discharged N prerequisite
    reproduced the shape on a second gate. codex-worker-1 found and proved it, and named the repair
    I had not seen. **THE DECISION IS STILL NOT MINE**: either a live non-gate prerequisite makes
    planning false and populates a typed blocker, or the schema states why it does not, and which
    is right depends on whether a planner can write tickets against a gate whose non-gate
    prerequisite is unmet — a question about how this packet is CONSUMED rather than about its text.
    **But declining to overstate is not deciding.** `planning_verdict` is three-valued —
    `plannable`, `unknown`, `blocked` — and `planning_unknown_from` names the gates the unknown
    comes from. **That field had ONE input for a SHA and lied by omission**: a live OWNER CHOICE
    also leaves planning unestablished, and G3 reported its unknown as coming from G1-B alone while
    `daw::Watchdog`'s fate and R16's resume mechanism were both unchosen — so resolving G1-B would
    have flipped G3 to `plannable`. Both inputs feed it now, and neither is a ruling here: this
    item's question is whether an unmet prerequisite makes planning FALSE and populates a typed
    blocker, which is the `blocked` state and is untouched.
    `plannable_with_dependencies` is REMOVED rather than widened: a tri-state string in
    a boolean's key reads as TRUE in every consumer written against `/5`, so a meaning that no
    longer fits its type is deleted at a schema bump where the break is loud. Whoever rules turns
    G1-B's PREREQUISITE half into `plannable` or into a typed blocker; **it does not turn any gate
    plannable on its own**, because every gate also carries or inherits an OWNER CHOICE, and this
    sentence said "the five unknowns" for one SHA while G0-A's and G3's causes sat beside it —
    codex-worker-1 caught it in the commit that introduced the second input. Whoever rules must also
    add the cross-record control, since nothing yet fails when a prerequisite and a verdict disagree. **Scoping matters here and I got it wrong twice, in opposite directions. Filed as
    `all` and as a BLOCKER for one draft, it attached to every gate and drove the
    acceptance-decidable set to empty — an item scoped `all` is a claim about every gate, and this
    is not one: the defect leaves no gate's acceptance criteria unmet, it makes a PLANNING field
    unreliable. Then, scoped correctly, it carried no kind marker, because the old rule made the
    marked set EQUAL to the blocker set and erased the work-kind of every open non-blocker. Kind
    and state are orthogonal — kind says what closes an item, blocking says whether a gate waits
    on it — so every blocker carries a kind and a non-blocker may. That relaxation does not make
    the marked population COMPLETE, and open item 39 (all) carries what it leaves: a kind is
    AUTHORED, an empty one means UNCLASSIFIED, and sixteen active open items are.** Both times the checker
    refusing was the derivation working. PACKET work.

39. **all** — ⟦PACKET⟧ **NOT BLOCKING. The kind vocabulary does not span the open set, and
    relaxing `kind == blockers` to `blockers ⊆ kinds` did not make the marked population
    complete.** Sixteen active open items carry no kind: **UNCLASSIFIED OPEN ITEMS (1, 2, 3, 4, 7,
    12, 14, 15, 16, 17, 20, 22, 25, 30, 31, 32)**, derived from the item heads and compared against
    this restatement. codex-worker-1 named items 1-4 and offered the choice: classify all open work,
    or narrow the stated contract. **I have narrowed it, and the reason is this packet's own
    sentence — a classification cannot be read out of prose that was not written to carry it.**
    Marking sixteen items in one pass by skimming their heads is that mistake at scale, and the
    check can catch a blocker with no marker while being unable to catch a blocker marked wrongly,
    so a wrong kind costs more than an absent one. What was fixed instead is the SILENCE: an empty
    `kind` now means UNCLASSIFIED rather than being indistinguishable from "no work kind", the
    population is emitted as a list, counted, and equality-gated against this sentence. **Item 22
    is the evidence that the vocabulary itself is short**: its content is that fixture definitions
    and ack-census counts have never been run, and the action it asks for is a REVIEW against a
    fixture — neither a product edit nor a packet edit. Whoever classifies this population must
    either place item 22 in one of the two kinds and say why, or author a third. Doing it well
    means reading each item against its gate, which is why it is filed rather than guessed.
    PACKET work.

# Provenance of this packet's own numbers

Every count is stated as RAW → RULE → IN SCOPE, so that the command reproduces the raw figure and the
rule reproduces the rest; and every count is a floor where a runtime value defeats the extraction.
**6 populations are HAND-CLASSIFIED and exempt from that sentence**, each carrying a marker and
open item 25 (all). **And of the 14 RAW claims, 13 of them apply their RULE BY HAND** — the command
returns the raw figure and a stated subtraction reaches the in-scope one — while none now carries its rule inside the command without also subtracting. That 12 is the honest size of what this gate
cannot decide: it checks every subtraction's arithmetic and none of their justifications, and the
one time a justification was wrong (G1-A's five ready-flag operations counted as four) the
arithmetic was perfect. The exact review reached this by noticing G4's dispatch split was
hand-classified while the provenance paragraph claimed only five exceptions; the general form is
that a claim can be COMMANDED at the raw level and hand-made at the split, and counting only fully
uncommanded populations misses every one of those. The previous version of this paragraph said no population was exempt, which was
false when written: the exact review named G0-A's and G4's semantic groupings while I was quoting
that sentence back at reviewers as the packet's strongest claim. The repair is not a softer
adjective — the exceptions are marked in place, counted, and the count is checked against this
number, so a sixth cannot appear silently. Two were: G0-B's member counts, obtained by reading both declarations, and
G2-A's 51 correlator sites, carried from the exact review on attribution. Both are now derived by
printed commands, and deriving them paid twice — the member commands localise the whole 66-vs-65
difference to `EventEntry`, and the correlator command refuted the author's competing figure of 56,
which had counted mentions where the claim is call sites.
The predecessor's version of this paragraph said every count was command-produced while two of its own
populations said "counted by reading" and "selected by hand" — a universal claim contradicted inside
its own document, which is the third instance of that shape in this lineage and the reason the
provenance paragraph is now written last, against the finished text. Counts corrected from the predecessor, in place
and named: `commandMutatesDocument` has **93** case arms, not 186; the mailbox census is **7 live of 8
syntactic**, having been stated as 6 and then as 10 — the first omitted the three RT loads and the
direct load, the second added a dead `Watchdog` read and a line that is not a load;
`UiClipRejectPayload` uses **18 of 40 bytes with 22 free**, not "22 of 40"; and a stride-overflow
premise that cannot be stored in a `uint32_t` had propagated into five places including a PASS bullet.

Two methodological failures are recorded because both produced findings or claims that were not real.
First, a verification stage of this packet's drafting passed each 57k–74k character section to its
checker truncated to 9,000 characters; the checkers reported — correctly for what they were shown —
that the sections carried no PASS conditions, no test and no evidence, and roughly 67 items were
phantom. The tell was that two of the author's own outputs disagreed about the same field and the
disagreement was averaged over rather than resolved. That stage was re-run against the complete text
and all eight gates confirmed the earlier report was an artifact; no conclusion here rests on it.
Second, the predecessor promised a seven-element record for every gate and delivered it for one,
because content was compressed under cost pressure while the claim about the content was left
standing. Both failures have the same shape as the defects this packet exists to specify against, and
they are recorded rather than repaired silently.
