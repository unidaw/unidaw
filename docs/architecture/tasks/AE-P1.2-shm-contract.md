# AE-P1.2 — SHM contract phase packet

Successor to AE-P1.1, which is FROZEN at `ba88bcb4657b62bdfc752d338d877e139e212ca6` and is not
amended by this packet. P1.1 governs how an acceptance packet states and checks its own claims; this
packet applies that machinery to the shared-memory contract itself.

**Product under review is FROZEN** at `75c6f0646417828641e43287c260bea3d38b5a6f` (tree
`699abfe8f72e597cfd1b6fb3da93ee7f35aa7fef`). Every citation in this document is a line at that SHA,
read from a pinned read-only checkout. This packet SPECIFIES and does not implement: no product edit
is proposed here, and none has been made.

## Provenance of the findings

Eight findings arrived from an independent semantic audit of the frozen product. Each was then
verified against the same SHA by a separate reader, and each confirmation was attacked by a refuter.
That pass changed the input materially, and the packet records the change rather than the original:

- **F8 was REFUTED as a live defect.** Two patcher exports do lack an `abi_version` guard, but no
  sequence at this SHA delivers a wrong version to them. The gate is re-scoped to what is actually
  missing — a `PatcherContext` layout pin — with the two guards demoted to hardening.
- **F1 moved CRITICAL to HIGH**, bounded to three chain shapes, because the default path is already
  gated by the very mechanism its gate generalises.
- **F4 was argued down to LOW and that was WRONG.** The reviewer's rebuttal held on the code: the
  argument for LOW was a threat-model answer to a robustness claim. It stands at HIGH for the C++
  host boundary.
- **F3's reclamation** belongs to the C++ consumer, not the Rust side as first reported.
- **F7's invariant was reworded**: a caller does not learn that a *different command* applied; it
  ADOPTS ANOTHER COMMAND'S TERMINAL OUTCOME for its own.

Severities of record: F1 HIGH, F2 HIGH, F3 MEDIUM/MEDIUM-HIGH, F4 HIGH (C++ host boundary) with the
Rust UI attach gap LOW, F5 MEDIUM-HIGH, F6 HIGH, F7 MEDIUM-HIGH, F8 re-scoped.

## Gate sequence

    G0-A ∥ G0-B  →  G1-A ∥ G1-B  →  G2-A  →  G2-B  →  G3  →  G4

**No downstream end-to-end success waives a failed primitive gate.** A green G4 over a red G1-A means
the G4 fixture did not exercise the primitive, not that the primitive is sound. Every test SHALL
expose deterministic barriers; a timing or stress result is not evidence, and this packet does not
accept one in place of a transition test.

## What this packet is, and what it is not

Each gate below states an INVARIANT, the POPULATION it quantifies over with the command that produced
that population and the FLOOR below which the extraction is known blind, a FAILURE MODEL, a
DETERMINISTIC TEST, PASS conditions each carrying the observation that REFUTES it, STATIC CHECKS for
what no runtime observation can decide, and a REVIEW REGISTER of obligations no mechanical check can
carry.

Three rules govern the gates, and each exists because this packet's drafts violated it:

1. **A PASS condition that cannot fail is not a PASS condition.** Every bullet carries `REFUTED BY`.
   A bullet satisfied by the pinned code, or by an implementation that computes a comparison and
   discards it, was deleted and its obligation moved to the review register.
2. **A population must be enumerated, with its extraction and its floor.** "Every callsite" is
   forbidden. Where a name-grep is defeated — G2-A's chord site passes a runtime-computed
   `commandType` — the gate says so and states the count below which the extraction is blind rather
   than the code safe.
3. **A gate must not be satisfiable by a different mechanism than the one intended.** Where a bullet
   was satisfiable by echoing a field, or by an engine-minted counter where the point was a
   sender-minted one, the mechanism is named inside the bullet.

---

## G0-A — Validated mapping geometry (C++ engine side of a host-created segment)

**Severity** HIGH (C++ host boundary). The analogous Rust UI attach gap is LOW and is deliberately
NOT in this gate; see the register.

**Scope.** GOVERNS the engine's consumption of a mapping created by a hosted-plugin child:
`apps/host_controller.cpp`'s attach path and every engine-side non-test access reached through
`HostController::shmHeader()/mailbox()/shmSize()/sharedMemory()` or the `TrackInfo` copies at
`apps/engine_consumer.cpp:648-672`. DOES NOT GOVERN: the engine's own UI segment
(`apps/engine_ui_shm.cpp`), `ui/daw-bridge/src/control.rs` attaching that segment, the child's own
accesses, or the test harnesses at `apps/phase2_tests_main.cpp:184/:197` and
`apps/phase3_tests_main.cpp:164/:177`. Those exclusions are listed so the gate does not silently
widen to the ~21 `ui*Offset` fields of `ShmHeader`, which are a different producer and a different
trust direction.

**Invariant.** The engine must not load or store through a pointer into a host-created mapping unless
the whole byte span of that access lies inside the object, decided against a length the engine
produces WITHOUT trusting the peer.

- **(a) Length.** The `mmap` length, and the number every later host-segment bounds check is measured
  against, is `S_expect = daw::sharedMemorySize(H, ...)` with `H` filled from the engine's own
  `HostConfig`. `response.shmSizeBytes` may appear in `host_controller.cpp` only as the left side of
  an equality test against `S_expect`. **`fstat` st_size is REQUIRED as a floor** (refuse if
  `st_size < S_expect`), not optional: without it an object truncated anywhere between
  `sizeof(ShmHeader)` and `S_expect` still faults on its last page, which is failure-model bullet 2.
- **(b) Floor.** Before any `ShmHeader` member is loaded the mapping must be known to be at least
  `S_expect` bytes of real object.
- **(c) Regions decided on the INPUTS, not the result.** Every region address comes from one
  validated-layout computation whose inputs are checked. A range test applied to an
  already-computed offset is not sufficient: a `uint64` `audioOutOffset` near 2^64 wraps
  `auxOutputPlaneOffset` SMALL and passes the end-of-region test at
  `apps/engine_consumer.cpp:775-776` and the span rule at `apps/engine_rt_helpers.cpp:258`.
- **(d) Order.** No `ShmHeader` member other than magic/version is loaded before the magic/version
  comparison, and no derived pointer is published to `shmView_`/`shmBase_`/`shmHeader_`/`mailbox_`
  before every gate passes. At pin both are violated: the field load is `host_controller.cpp:291`
  against a gate at `:298`, and all four publications happen at `:294-297`. **Order is not observable
  from a return value** — a refused attach unwinds via `disconnectInternal(false)` and leaves the
  accessors null either way — so (d) is decided statically and never by the fixture.

**Populations, exact at 75c6f064, each with its command.** Eight regions the engine addresses; twelve
raw region derivations outside any validator; seven bounds checks measured against the child's
number; three ring constructions over a host-created mapping. **Mailbox read sites: TEN, not six** —
the six reached from `HostController::mailbox()` plus the four reached by pointer-passing through
`TrackInfo::completedBlockId` (`apps/engine_consumer.cpp:655` into
`apps/engine_audio_callback.h:284/:328/:951`, and the sidechain load at
`apps/engine_produce_block.cpp:910`). The three RT-thread loads are the highest-consequence reads in
the set and an earlier census called SIX EXACT while omitting them.

**Floor.** The region, derivation and ring censuses are name-greps and are exact because the names
are the mechanism; the mailbox census is a pointer-flow census and is a FLOOR of ten, not a
certainty — a `TrackInfo` copy through a new path is invisible to it.

**Deterministic test.** Three parts, because part A structurally cannot decide the rest. Part A drives
`HostController::connect()`/`launch()` against a stub host installed via the existing `DAW_HOST_BINARY`
hook (`host_controller.cpp:124-128`), one row per geometry mutation. Part B drives the region half,
which `connect()` never reaches — the mailbox poll, both event rings and the key ring are constructed
after attach. Part C is static. The stub links `apps/ipc_io.cpp` rather than hand-rolling framing,
because `recvHeader` rejects on `kControlVersion` and a hand-rolled constant would turn every row into
a handshake refusal that looks like a pass.

**PASS conditions.**

1. **Length provenance.** `mmap`'s length and `SharedMemoryView::size` are `S_expect`;
   `response.shmSizeBytes` appears only in the equality test. *REFUTED BY* the ratchet finding it in
   any other position.
2. **Row A1 and only A1 attaches.** *REFUTED BY* A1 refusing for any reason. This is the anti-trivial
   half: without it the table is satisfied by an engine that refuses every host.
3. **Every refusing row refuses by the mechanism it names**, read from a typed
   `HostController::lastAttachError()` whose enum includes `MmapFailed` — three refutations below
   depend on observing that value and an earlier enum omitted it. *REFUTED BY* A5 refusing as
   `MmapFailed` rather than `SizeMismatch`.
4. **The floor exists at S_expect.** A row whose object is `S_expect - 4096` refuses with
   `ObjectTooSmall` and the forked child exits normally. *REFUTED BY* the child dying on a signal —
   which is the pinned behaviour — or by the row refusing with `MagicMismatch`.
5. **The region population is routed through one validator**: raw region derivations outside
   `validateHostLayout` number 0. The validator lives in `apps/shared_memory.cpp` so the ratchet's
   path exclusions can express "outside the validator"; without naming its home the count can never
   reach zero. *REFUTED BY* the command returning any line outside it on the delivered tree.
6. **The validator refuses on the INPUT.** The overflow row is an offset-field wrap
   (`audioOutOffset = 2^64 - k`, so `auxOutputPlaneOffset` wraps inside the header) and is refused
   before any region end is computed. The earlier row posited `channelStrideBytes` near 2^58, which
   is **unstorable** — the field is `uint32_t` at `apps/shared_memory.h:185` — and that impossible
   premise had propagated into five places. *REFUTED BY* the wrap header returning a valid layout, or
   being refused as `RegionOutOfRange` (the result was checked, not the input).
7. **The host key ring is in the population.** The row where the mailbox exactly fits and the derived
   key ring does not refuses with `RegionOutOfRange(HostKeyRing)`. *REFUTED BY* it attaching.
8. **Order is decided statically and the ratchets bite.** S1-S3 pass on the delivered tree and FAIL on
   the pinned file. *REFUTED BY* any ratchet passing against pinned `host_controller.cpp`: a ratchet
   that passes on the known-violating artifact is matching a name, not an order.
9. **The negative control flips the verdict and proves it ran.** With `S_expect` replaced by
   `response.shmSizeBytes`, rows A2-A12 fail and the control prints a build stamp identifying the
   sabotaged binary. *REFUTED BY* the suite still passing, or a passing control with no stamp — which
   is indistinguishable from a sabotage that never reached the binary.

**Static checks.** S1 pre-gate field load; S2 pre-gate publication; S3 single-validator routing, whose
exclusion set is FIVE path exclusions plus one pattern (`uiShm`) — the pattern is the accident-prone
kind and is named as such rather than disclaimed; S4 ratchet mutation test, each of S1-S3 run against
a copy with the violation restored; S5 `response.shmName` reaches `shm_open` only after a
length-bounded read.

**Review register.** The reviewer SHALL confirm: the owner's ruling on post-attach shrink, which no
validation closes because the child keeps its fd; the expected verdict for a valid object under a name
the engine did not choose; refuse-versus-degrade for a bad ring offset; that `mmap` past the object
succeeds on this platform, since nothing here was executed; that the fixture asserts `offsetof` rather
than trusting this packet's hand-computed byte 96; that the enumerations were RE-DERIVED at the
delivering SHA rather than copied; that all TEN mailbox readers take their pointer from the validated
layout; whether `shmHeader()` can be null at `apps/daw_engine_main.cpp:955`; that the Rust UI attach
gap has been allocated to its own gate; and that the unbounded test harnesses are brought onto the
validator or removed, so they do not stand as the pattern to copy.

**Dependencies.** None. First gate.

---

## G0-B — PatcherContext layout pin

**Severity** re-scoped. The finding as received — two patcher exports missing an `abi_version` guard —
is NOT a live defect at this SHA, and the packet records why rather than quietly keeping it: all seven
`PatcherContext` sites are `daw::PatcherContext ctx{}`, which applies the NSDMI at
`apps/patcher_abi.h:114` before their explicit assignment; a repo-wide grep shows `abi_version` is
never sourced from SHM, a project file or a UI command; `patcher_rust` is a staticlib with no `dlopen`.

**Why the guard is not the mechanism.** `abi_version` is field 0 of both mirrors, so it catches only
"layout changed AND version bumped AND artifact stale". In the sibling case — layout changed WITHOUT a
bump — the three GUARDED exports corrupt identically: `patcher_process_euclidean` passes its version
check at `patcher_rust/src/lib.rs:287` and then takes `event_buffer`/`event_capacity`/`event_count`
from shifted bytes at `:290-293`, writing 64-byte `EventEntry` records through them. The live risk is
same-build C++/Rust layout drift without a version bump, and nothing pins it:
`apps/patcher_abi.h:172-188` static_asserts `EventEntry`, `MusicalLogicPayload`, `PatcherLfoConfig` and
`HarmonyEvent`, and pins nothing about `PatcherContext` itself; the Rust const-assert block at
`lib.rs:35-38` covers only `HarmonyEvent`.

**Invariant.** For the eight types crossing the patcher node ABI, the C++ definition as a compiler
lays it out and the hand-written Rust mirror must agree on `sizeof`, `alignof`, and per member: byte
offset, byte size, normalised name, and normalised TYPE FORM — integer width AND signedness, float, or
a pointer spine with per-level mut/const and a normalised terminal type. Type form is in the tuple
because offset, size and name are together blind to `float sample_rate` becoming `uint32_t`.

**Mechanism.** ONE generated set of numbers, not two hand-written ones: C++ offsets from bindgen over
`apps/patcher_abi.h`, Rust offsets from `core::mem::offset_of!`/`size_of` over the mirrors, compared
mechanically. The comparison must gate the artifact `daw_engine` links, and must re-run on a C++-only
edit through BOTH the cargo leg and the cmake leg — today's wiring re-runs on neither.

**PASS conditions** (abridged; each carries its refutation in full in the gate table): one generated
set of numbers; the comparison gates the linked artifact, *refuted by* a Rust-side-only drift that
still links; a C++-only edit re-runs the comparison on each leg, *refuted by* widening
`uint32_t num_frames` to `uint64_t` at `patcher_abi.h:120` and the build succeeding; member-level
comparison including type form, *refuted by* `float sample_rate` → `uint32_t` exiting 0 when nothing
moves and nothing resizes; the type population derived by two independent methods that must agree; per
type member counts derived, printed and ratcheted; the single permitted one-sided member admitted by
byte-disjointness and COUNTED PER TYPE rather than by one global integer — a global counter lets a
deletion and a padding-resident insertion in the same commit cancel.

**Known open, carried to review** (four items I did not close): the generated header's effect on the
documented `-DDAW_BUILD_PATCHER_RUST=OFF` build for six unconditional targets, with no stated path or
target-ordering edge; the declaring macro invalidating the `#[repr(C` grep that the gate's own
population floor depends on; the asserted ">= 600 mutation" floor, which has no derivation and which I
count near 551 over the real 131 members, so the battery would fail its own floor permanently; and the
mutation floor being a fifth pinned integer with no owner.

**Dependencies.** None.

---

## G1-A — Ring reservation, publication and reclamation

**Severity** MEDIUM/MEDIUM-HIGH composite.

**Invariant.** A slot in a `RingHeader`-fronted ring carries `ready == 1` if and only if the entry in
it was written in full by the producer that most recently reserved it; no consumer may read a slot's
bytes as data, or release the slot back to the producers, in any other state. Five obligations, not
three — the draft's three did not cover its own failure model, and `ringSkipStalledSlot` CONFORMS to
the draft's obligation (b) while committing the defect.

**PASS conditions.** Populations re-derived after the change rather than copied; the read guard in
`drain_ui_out` loads the slot's own `ready` with acquire ordering and stops the drain at zero;
**disarm on delivery asserted as a 1→0 transition** in the test-owned buffer, *refuted by* observing
`entries[0].ready != 0` after a drain that returned 1 — the pin never writes `ready`; the half-fix is
rejected, *refuted by* the second lap returning entries; discard is conditional, so
`ringSkipStalledSlot` leaves cursor and entry untouched when the slot became ready between the stall
observation and the skip; order and ordering decided on the source; index discipline across all twelve
sites; reservation values do not recur after a lap; and the unguarded C++ reader at
`apps/device_chain_ui_live_tests_main.cpp:110-113`, which walks `ringStd` by raw index with no `ready`
load on a ring with six live producers, is closed.

**Corrections applied.** The claim of "uninitialised `EventEntry` / indeterminate flags" is deleted —
`EventEntry` has an NSDMI. The ABA severity sentence is narrowed: `keyRing` has one writer and
`ringUiEdit` has none, so ABA is unreachable on both 64-slot rings and the reachable population is the
multi-producer rings only. The lap fixture stores `(k+1) & mask`; as drafted it stored `write_index=8`
on a mask-7 ring, a state the producer cannot create, and read one entry past the array.

**Known open:** the entry-address grep returns 21 lines, 12 of them plugin-cache reads, so the ring
filter must be stated in the command or the population bullet is measuring the wrong set.

**Dependencies.** G0-A.

---

## G1-B — Coherent snapshot and response publication

**Severity** HIGH, and the weight is sub-claim (C): a same-device parameter query deterministically
accepts the PREVIOUS answer. It is silent and invisible at the call site because the stale answer's
echo IS the requested track and device, the function's own doc comment promises the opposite, and an
LLM agent keys `modulate`/`set_param` on the uid it returns.

**Invariant.** Every answer a client takes out of the UI segment must be (i) matched to the client's
own question by a **sender-minted** identity written into the request and echoed back verbatim — not
by the engine's version counter, not by non-emptiness, and not by the engine re-stating the address
the client asked about; and (ii) a complete image of exactly one execution of the writer.

**PASS conditions.** The four device-params consumers accept only on the sender-minted echo; the
stale-answer fixture reports failure at the step that must fail and success at the step that must
succeed; no minting expression maps two questions to one identity; the four seqlock opens each carry a
release fence between the odd store and the first payload store, decided on the source AND the emitted
code, never on an execution; the two in-place rewrites carry an in-flight marker for the whole rewrite
and their readers consult it; **an abandoned publication is distinguishable from an unanswered one
within a bounded number of observations, and the reader reports which it saw** — this replaces a draft
bullet that only required the in-flight condition to be "observable"; no reader loads a peer-written
word through a plain non-atomic load, and the bullet names the two that do today; and a mechanical
echo ratchet over the send sites fails under a named mutation.

**Known open:** three population reconciliations — the send-site count stated variously as 16, "the
sixteen", and a printed list of 15; a region the scope omits but the population calls one of "the only
two"; and two extraction recipes that do not reproduce their own lists.

**Dependencies.** G0-A, G1-A, and the production atomic size/alignment assertions.

---

## G2-A — End-to-end command identity

**Severity** MEDIUM-HIGH. **Wording of record:** the defect is NOT that a caller is told a different
command was applied. It is that a caller **adopts another command's terminal outcome** for its own,
because correlation is by `(scope, command, base)` with no command id.

**Invariant.** A client's terminal verdict on a mutating command may be adopted from an engine-published
record only when that record carries an identity **the sending process minted** for that one command
instance and the engine copied through unchanged. The mechanism is named so an engine-side
discriminator does not empty the clause: a correlator presented with a record whose every content
field equals its own command, but whose identity it did not mint, must return not-mine.

**Population, and why it is not what it looks like.** `commandMutatesDocument`
(`apps/engine_command_mutates.h:46-236`) has **93** case arms, 69 returning true — an earlier draft
said 186, off by 2x in the section whose credibility rests on its counts. But the sub-population that
can produce an ADOPTABLE outcome record is **nine**, not 69: `WriteNote`, `DeleteNote`, `WriteChord`,
`DeleteChord`, `RevertPlacementOverrides`, `SetAutomationTarget`, `SetClipText`, `WriteHarmony`,
`DeleteHarmony`. `tools/version_arbiter_check.sh` says so in its own header: for the other families
the classification is dead code no counter is ever chosen by. For those ~60 types no `ClipRejected` is
ever emitted, so "no correlator may adopt a foreign record" is vacuous there, and requiring every
outcome record to carry an identity would mean inventing emit sites the code says do not exist.

**Extraction floor.** The nine are NOT enumerable by name-grep: the chord site passes a
runtime-computed value — `const auto commandType = payload.commandType == ... ? WriteChord :
DeleteChord;` then `requireMatchingClipVersion(chordPayload.baseVersion, commandType, ...)`
(`apps/engine_note_commands.cpp:136-142`). Re-running the extraction by hand yields FIVE names against
that script's own blindness floor of `-lt 7`. Any bullet quantifying over "every arbitrated command"
needs a predicate that survives a runtime-computed `commandType`, and the floor is stated because
finding fewer means the extraction broke, not that the code got safer.

**PASS conditions.** The outcome record carries the id the sender minted, copied and not computed; the
id is unique across sending PROCESSES, not only within one, and the mechanism is named because a
per-process counter is insufficient; no record-reading correlator adopts a foreign record and each
still adopts its own; the counter-only decisions are REPLACED, not stamped; the decision rests on
identity, not on a consumable window position, *refuted by* deleting `.skip(before_len)` changing any
verdict; the superseded room claim is gone and every surviving room claim carries a recomputed byte
count — the assertion that "the 40-byte payload has no room" is not established by the code, since
`UiClipRejectPayload` uses 22 of 40 bytes and declares `reserved2[5]`
(`apps/event_payloads.h:1581`); the retry is gated on identity, not on the reason code, because
`retry_stale` is the DEFAULT (`ui/daw-cli/src/main.rs:2979-2981`) and the false-refusal path silently
re-applies an edit that already landed, splitting one user action across two undo entries — a doc
comment at `:1143-1145` records that this exact re-send already shipped once and broke redo; and
**Unknown is reported distinguishably from Applied**, which is not an identity bullet and is present
because without it an implementation answering Unknown to everything satisfies every other bullet and
preserves the entire harm: `ClipOutcome::Unknown` is documented "Treated as applied" and callers
return 0.

**Known open:** the Layer-1 fixture arithmetic (11 journal lines, not 6, and ids legitimately repeat,
so a correct implementation fails the gate's only runnable integration assertion); a population
mismatch where 22 of 27 correlator sites read records no bullet requires to carry an id; an omitted
correlator family (`refused_or` → `await_refusal`, 24 call sites, one of them a governed command); and
a check blind to the BATCH note branch.

**Dependencies.** G1-A, G1-B.

---

## G2-B — Recovery readiness and state publication

**Severity** MEDIUM-HIGH. The fix is a **lock scope**, not a novel state machine: the identical
sequence one file over, `apps/engine_track_setup.cpp:377-418`, already holds the lock across the whole
publish. The gate is written against that existing correct pattern.

**Invariant, stated as the observable.** No `ProcessBlock` is dispatched to a host whose per-slot
bypass differs from the authored chain, where "authored" means the vector as copied at
`apps/daw_engine_main.cpp:1113` under `trackMutex` — that copy is the only referent that exists,
because the guard is released at `:1114` while the `SetBypass` loop runs at `:1116-1124` under
`controllerMutex` alone, so the chain can change across the gap.

**PASS conditions.** The decider contains no clock: with the worker provably parked inside the recovery
hook, an RT-shaped probe records a refusal; the blocking shape covers the offline render and master
bus, which the try_lock probe cannot, because a try_lock refusal is also what contention looks like;
the swap trap is a conjunction no single-line reordering satisfies; liveness is scoped to the path
that reaches the publish; the witness set converts a vacuous green into a FAIL by requiring
"recovery-hook-entered" exactly once and the cycle to end with `hostGaveUp` false; and the control
sabotage restores only the ordering and must flip the decider.

**Known open:** the fix class this gate admits requires `applyHostBypassStates` to stop taking
`controllerMutex`, i.e. a caller-holds contract — that self-deadlock must be stated in the
dependencies and the register; the swap trap depends on an unratcheted guard at
`apps/daw_engine_main.cpp:1107-1109` and needs either a static check pinning it or a restatement as a
disjunction; and the fixture must forbid the offline thread from attempting its blocking acquisition
before the RT probe has appended its verdict, or the PASS token is producible by contention from the
packet's own probe.

**Dependencies.** G0-A, G1-A, G1-B, G2-A.

---

## G3 — Per-host failure containment

**Severity** HIGH. Blast radius is the entire session.

**Invariant.** For a NON-MASTER track — a member of the `tracks` vector snapshotted at
`apps/daw_engine_main.cpp:962-969`, which excludes the master constructed at `:449` — a hosted plugin
that stops advancing its `completedBlockId` while its control socket stays writable must, after a
declared bound N counted in OBSERVATIONS OF THAT HOST, have `hostReady` stored false by an autonomous
engine path, and block production for the remaining tracks must continue.

**Verified at pin.** The in-flight/`numBlocks` backpressure at
`apps/engine_producer_thread.cpp:225-289` is global, not per host, and `Watchdog::check`
(`apps/watchdog.h`) has no production caller.

**PASS conditions.** Layer 0 decides the experiment rather than the engine's lifespan; the Layer 1
boundary is exact and the bound is COUNTED, not timed — not evicting at observation N-1 and evicting
at exactly N; **the counter resets on advance, not on catching up**, so a host that owes, advances one
block per observation and stays `numBlocks-1` behind for 3N observations is never evicted; **absence
is not non-advancement**, covering the try_lock drop at `apps/engine_producer_thread.cpp:231-233` that
a ten-second Zebra2 load causes; the mutation control requires the new boundary assertions to go RED
while all 21 existing `CHECK` statements stay GREEN; the session survives on a surface no current
check touches; the eviction lands on `hostReady`, not merely on the producer's vector; byte
reproducibility with the fixture condition that makes it mean anything; and a negative control that
must exit 2 with the difference named in advance, restored by cp-backup rather than `git checkout --`.

**Known open:** a bound whose denominator is structurally 1 must be reconciled; the fixture must set
`DAW_ENGINE_DEBUG_STALL`, since every stall line is gated on it; and a static check currently forbids
the change its sibling requires.

**Dependencies.** G2-B. **Note:** the gate as received declares a dependency on a document that does
not exist in the pinned tree; the dependency is on the G2-B gate of THIS packet, and that is stated
here rather than left as a dangling reference.

---

## G4 — Segment and routed-audio ownership

**Severity** HIGH, not CRITICAL, and the reason is load-bearing rather than cosmetic: the default path
is already correctly gated by the very mechanism this gate generalises.
`apps/engine_audio_callback.h:328/:347` and `apps/engine_master_render.cpp:80-95` both wait on
`completedBlockId`, and the sidechain read at `apps/engine_produce_block.cpp:907-919` indexes by the
COMPLETED id. Those are counter-examples proving omission rather than design, and they bound the blast
radius to three shapes: chains interleaving VST and non-VST devices, a patcher audio node after a
plugin, and track-to-track routing from a plugin-bearing track.

**Invariant.** For every dispatch, identified by the quadruple (host generation `g`, `blockId b`,
`segmentStart s0`, `segmentLength sl`) — NOT by the loop ordinal, because segmentation is recomputed
from `trackState.chainDevices` on every block (`apps/engine_produce_block.cpp:660-716`) so a chain edit
renumbers ordinals while `(s0,sl)` still names the same plugins — the bytes of that host's INPUT plane
slot for `blockIndex = b % numBlocks` are owned by the host from dispatch until that exact segment
acknowledges consumption; a following patcher node or segment may read only the matching completed
output; and track routing consumes a completed block by explicit id.

**Verified at pin.** Dispatch is one `MSG_DONTWAIT ::send` (`apps/host_controller.cpp:366`,
`apps/ipc_io.cpp:105-113`) with no reply read. Segment N>0 memcpys the un-awaited out plane over the
same `audioIn` slot the host is still reading for segment N-1
(`apps/engine_produce_block.cpp:1030-1033`, `:940-945`); a patcher audio node after a plugin reads
`audioOut` for a block the plugin has not produced (`:1108-1113`); routing copies the same un-awaited
plane (`:1147-1154`). The host publishes `completedBlockId` only on the last segment
(`apps/juce_host_process_main.cpp:1069-1087`), so **there is no per-segment signal to wait on even in
principle** — which is why the invariant demands an ack keyed on the quadruple rather than a wait on
the existing mailbox.

**PASS conditions.** An ack census with non-vacuity first, one ack per request and the count equal to
what the fixture's chain implies; ordering in the merged log, `recv_request` before `read_input` before
`ack`, and every next-segment `write_input`, patcher `read_output` and routing read strictly after;
input immutability observed in the BYTES, not in the schedule; full-identity comparison across all four
components, *refuted by* any consumer that reads when one component is wrong and the other three are
right; generation engine-minted and host-echoed; segmentation-isolating content; stale-lap priming, so
the previous lap's content is non-zero and distinct from the value it could masquerade as; **wall clock
out of the decision** — re-running every schedule with holds multiplied by 1, 10 and 100 produces the
identical verdict and byte-identical WAVs, *refuted by* any verdict that changes with the multiplier;
and the sidechain carve-out preserved.

**Known open:** the ack census's expected dispatch counts need correcting and the master clause
inverting to "exactly one per `masterBlockId`"; and the negative control must be respecified against
"the pinned tree PLUS this gate's instrumentation", because an unmodified pinned build cannot emit the
merged log at all.

**Dependencies.** All of G0-A, G0-B, G1-A, G1-B, G2-A, G2-B, G3. Final release gate.

---

## Implementation constraints (non-negotiable, backend 2026-08-10)

1. Production atomic **size/alignment/`is_always_lock_free` assertions** and the canonical checked
   `LayoutSpec` land FIRST, before any gate's fix.
2. **One coordinated `kShmVersion` bump** for layout/snapshot/ring changes; `kControlVersion` if the
   host ring or Hello changes; `kPatcherAbiVersion` separately, carrying
   `{magic, version, struct_size, status}`.
3. `SHM_LAYOUT.md` and the C++↔Rust generated parity are updated in the **same changeset** as the
   change they describe.
4. Barrier fixtures run on **macOS arm64 AND Windows x64**. A timing or stress result is not
   accepted in place of a deterministic transition test — and this project has a documented history
   of "zero underruns, therefore correct" conclusions that were wrong, so the rule is stated rather
   than assumed.

## Known open at this SHA

This packet is announced with the following NOT closed. They are listed because a packet that
presents a clean sheet it has not earned is the failure this lineage exists to prevent.

- **G0-B**: the generated header's effect on the `-DDAW_BUILD_PATCHER_RUST=OFF` build (six
  unconditional targets, no stated path or target-ordering edge); the declaring macro invalidating
  the `#[repr(C` grep the gate's own population floor depends on; a ">= 600 mutation" floor with no
  derivation, which I count near 551 over the real 131 members.
- **G1-A**: the entry-address extraction returns 21 lines of which 12 are plugin-cache reads; the
  ring filter must be in the printed command.
- **G1-B**: three population reconciliations — a send-site count stated as 16, as "the sixteen", and
  as a printed list of 15; a region the scope omits but the population calls one of "the only two";
  two recipes that do not reproduce their own lists.
- **G2-A**: Layer-1 fixture arithmetic (11 journal lines, not 6; ids legitimately repeat, so a
  correct implementation fails the only runnable integration assertion); 22 of 27 correlator sites
  read records no bullet requires to carry an id; an omitted correlator family with 24 call sites.
- **G2-B**: the admitted fix class requires a caller-holds lock contract that is not yet stated; the
  swap trap rests on an unratcheted guard.
- **G3**: a containment bound whose denominator is structurally 1.
- **G4**: ack-census counts, and a negative control specified against a build that cannot emit the
  log it is measured on.

## Provenance of this packet's own numbers

Every count here was produced by a command run against the pinned read-only checkout, and every
count is a floor where the extraction is defeated by a runtime value. Two counts in the drafts were
wrong and are corrected in place rather than silently: `commandMutatesDocument` has **93** case arms,
not 186; the mailbox read population is **ten**, not six, and the three omitted sites are the
RT-thread loads, which are the highest-consequence reads in the set.

One methodological failure is recorded because it produced findings that do not exist. An earlier
verification stage of this packet's own drafting passed each 57k–74k character section to its checker
truncated to 9,000 characters, and the checkers reported — correctly for what they were shown — that
the sections carried no PASS conditions, no test and no evidence. Roughly 67 items were phantom. The
tell was that two of the author's own outputs disagreed about the same field, and the disagreement was
averaged over rather than resolved. No conclusion in this packet rests on that stage; it was re-run
against the complete text, and all eight gates confirmed the earlier report was an artifact.
