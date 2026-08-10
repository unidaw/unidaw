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

Six blockers from the exact review are reconciled here:

1. **The per-gate record contract is now met for all eight gates.** Each carries population with its
   extraction command and floor, failure model, deterministic test, PASS conditions each naming their
   refutation, static checks, and review register. Nothing points at a table that does not exist.
2. **The open list is 20 atomic items, not 15 categories.** The four that compression swallowed are
   restored: G0-B's unowned mutation floor, G2-A's BATCH blindness, G2-B's probe-order false-green,
   and G3's debug-env requirement plus its self-contradicting static check.
3. **G0-A's mailbox census was wrong twice and is corrected with its method.** See G0-A.
4. **G2-B now covers mirror replay and readiness and `sendSetBypass` failure**, not bypass alone.
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

    G0-A ∥ G0-B  →  G1-A ∥ G1-B  →  G2-A  →  G2-B  →  G3  →  G4

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
- *Regions the engine addresses* — 8, exact. Every `ShmHeader` offset field read off a
  controller-derived header plus the two derived functions.
- *Raw region derivations outside any validator* — 12, exact. Command:
  `grep -rn -e '->audioInOffset' -e '->audioOutOffset' -e '->ringStdOffset' -e '->ringCtrlOffset' -e '->mailboxOffset' -e 'auxOutputPlaneOffset' -e 'hostKeyRingOffset' apps/`
  minus `_tests_main`, `juce_host_process_main.cpp`, `engine_ui_shm.cpp`, `audio_shm.cpp`,
  `shared_memory.*`, and the `uiShm` pattern.
- *Bounds checks anchored on the child's number* — 7, exact.
- *Ring constructions over a host-created mapping* — 3, exact.
- **Mailbox `completedBlockId` LOADS — 7 live, 8 syntactic.** Command:
  `rg -n 'completedBlockId' apps --glob '!*_tests_main.cpp' --glob '!juce_host_process_main.cpp' | grep -E '\->load|\.load'`
  returns 8: `engine_produce_block.cpp:910` (sidechain), `engine_audio_callback.h:284`, `:328`,
  `:951` (RT thread), `engine_consumer.cpp:762` (direct, via `shmView->completedBlockId`),
  `engine_master_render.cpp:83`, `engine_producer_thread.cpp:239`, and `watchdog.h:47`.
  **`watchdog.h:47` is DEAD**: `Watchdog::check` has no production caller — which is G3's own finding,
  so counting it as a live reader would make this packet contradict itself across two gates.

**Floor.** The region, derivation, bounds and ring censuses are name-greps and are exact because the
names are the mechanism. The mailbox census is a **pointer-flow** census and is a FLOOR of 7 live: a
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
mutation; the stub links `apps/ipc_io.cpp` rather than hand-rolling framing, because `recvHeader`
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

**Review register.** The reviewer SHALL confirm: the owner's ruling on post-attach shrink, which no
validation closes because the child keeps its fd; the expected verdict for a valid object under a name
the engine did not choose; refuse-versus-degrade for a bad ring offset; that `mmap` past the object
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
carry — is NOT a live defect at this SHA: all seven `PatcherContext` sites are `daw::PatcherContext
ctx{}`, applying the NSDMI at `apps/patcher_abi.h:114` before their explicit assignment; a repo-wide
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

**Population.** *ABI types* — 8. Command: `grep -n '#\[repr(C' patcher_rust/src/lib.rs` (8 hits:
lines 23, 40, 55, 68, 82, 89, 97, 107), cross-checked against bindgen's `allowlist_file` closure over
`apps/patcher_abi.h`. *Members* — 66 C++ / 65 Rust, counted by reading both declarations:
`PatcherContext` 26/26 (`patcher_abi.h:114-152` / `lib.rs:109-143`), `MusicalLogicPayload` 9/9, and
six further types. *One-sided members* — exactly 1: C++ `EventEntry::ready`, offset 60, size 4
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
8. The one permitted one-sided member is admitted by byte-disjointness and **counted PER TYPE**.
   *REFUTED BY* a deletion and a padding-resident insertion in the same commit cancelling — which a
   single global integer permits, and which the predecessor's version permitted.
9. Names join under a pure rule with no alias table, and the gate is RED at this SHA until a rename
   lands. *REFUTED BY* the check exiting 0 with `patcher_abi.h:75 reserved` and `lib.rs:86 _pad0` both
   unchanged.

**Static checks.** S-1 the generated table's include sits above `lib.rs:893`'s first export, a
position invisible from any run's output. S-2 the check's non-generated sources contain no literal
list of the eight type names. S-3 the one-sided admission is a byte-range disjointness predicate, with
no member name used as an admission key. S-4 name normalisation is a pure function of the input
string — no map or table literal keyed by a member name.

**Review register.** The reviewer SHALL confirm: that libclang's layout for the eight types equals
that of the compiler building `engine_core`, since the C++ numbers come from bindgen and the product
comes from the host compiler; whether Rust's whole-struct store `*slot = entry` (`lib.rs:157-158`)
writes `EventEntry` bytes [60,64), the C++ `ready` member; the ruling on the
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

**Population.** *Rings* — 8 production. Command: `rg -n '\->capacity\s*=' apps/ | rg -v '=='` returns
10, being the 8 plus 2 test fixtures. *Statements interpreting an entry's bytes as data* — 5. Command:
`rg -n '\.entries\[|->entries\[' apps/` returns 21, **of which 12 are plugin-cache reads**; the ring
filter must be in the printed command or the population measures the wrong set. *Read-cursor stores* —
4. Command: `rg -n 'readIndex\.store|read_index\.store' apps/ ui/`. *Index sites* — 12.

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
1. Populations re-derived AFTER the change, not copied. *REFUTED BY* any of the four commands
   returning a member this packet does not name.
2. Read guard in `drain_ui_out` on the slot's own flag, acquire ordering, stop at zero. *REFUTED BY*
   the drain returning 1 for an unpublished slot, or `read_index` moving.
3. Disarm on delivery asserted as a **1→0 transition** in the test-owned buffer. *REFUTED BY*
   `entries[0].ready != 0` after a drain that returned 1 — the pin never writes `ready`.
4. The half-fix is rejected. *REFUTED BY* the second lap returning entries.
5. Discard is conditional: `ringSkipStalledSlot` leaves cursor and entry untouched when the slot
   became ready. *REFUTED BY* the retirement being indistinguishable from a discard.
6. Order and ordering decided on the source. *REFUTED BY* S1–S3 passing on the pinned file.
7. Index discipline at all twelve sites. *REFUTED BY* any site whose index operand is neither a mask
   expression nor a local all of whose assignments are.
8. Reservation values do not recur after a lap, on both sides. *REFUTED BY* the raw write cursor
   returning to its pre-lap value.
9. The unguarded C++ reader at `apps/device_chain_ui_live_tests_main.cpp:110-113` — raw index, no
   `ready` load, on a ring with six live producers — is closed.

**Static checks.** S1 the ready-clear lexically precedes the cursor store at all four sites, same
function, no return or branch between. S2 the read guard's load is an acquire load of THAT SLOT's
`ready`, and the clear is a release. S3 the re-check and the advance are one step in the callee. S4
mask-in-expression at all twelve index sites.

**Review register.** The reviewer SHALL decide whether a producer publishing into a slot it no longer
owns after the 2-second grace is in scope; SHALL establish or explicitly decline the producer census
on `ui_out`; SHALL confirm no second consumer of `ui_out` exists, since the disarm is safe only under
the single-consumer contract; and SHALL confirm Fixture A compiles, nothing in the pin having been
built.

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

**Population.** *Seqlock opens and closes* — 4 + 4. Command: `rg -n 'seq\.store' apps` returns exactly
8, cross-checked per file with `rg -c`. *Request/answer readers* — 6 of 21. Command:
`rg -n 'pub fn read_' ui/daw-bridge/src/control.rs` returns 21; the 6 in scope were selected **by
hand**, which is stated because it is the one population here not decided by a command. *Call sites of
those six* — 16 production, 2 test, obtained per name and discarding definitions and three doc
comments.

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
7. No reader loads a peer-written word through a plain non-atomic load. **Two of the six fail today
   and the bullet names both**: `ui/daw-bridge/src/control.rs:944-949`. *REFUTED BY* either remaining.
8. An abandoned publication is distinguishable from an unanswered one within a bounded number of
   observations, and the reader reports which it saw. *REFUTED BY* a reader that returns the same
   value for both. This replaces a predecessor bullet requiring only that the condition be
   "observable", which any implementation satisfies.
9. A mechanical echo ratchet exists over the send sites and fails under a named mutation. *REFUTED BY*
   the ratchet passing when one site's echo check is deleted.

**Static checks.** S1 fence between open and first payload store — statement ORDER is invisible from
any return value, since a clean image proves nothing about the fence. S2 write order inside the
device-params body: identity words first (`:122-123`), `paramCount` last (`:169`), which is what makes
the stale answer readable as complete. S3 minting expressions, naming the two collapsing forms. S4
population-drift detectors asserting the three counts at the reviewed SHA.

**Review register.** The reviewer SHALL rule on how device-params acquires a request identity — a new
field on `UiDeviceParamsRegion` implies a `kShmVersion` bump; SHALL confirm whether the plain snapshot
copy at `control.rs:644-646` bracketed by the `ui_version` double check is acceptable; SHALL state
which thread owns each region's publication and confirm the device-params region and the audio clip
table are INTENDED to be single-writer; and SHALL decide what abandonment this gate defends against —
process death, an exception inside the body, or neither — since there is no early return between any
open and its close.

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

**Population.** *Arbitrated commands* — **9**, being the only commands that can produce an adoptable
refusal: `WriteNote`, `DeleteNote`, `WriteChord`, `DeleteChord`, `RevertPlacementOverrides`,
`SetAutomationTarget`, `SetClipText`, `WriteHarmony`, `DeleteHarmony`. Command: `grep -rn` for
`requireMatchingClipVersion(` and `requireMatchingHarmonyVersion(` over `apps/*.cpp` minus tests.
`commandMutatesDocument` (`apps/engine_command_mutates.h:46-236`) has **93** case arms, 69 returning
true — the predecessor said 186, off by 2× in the section whose credibility rests on counts — but for
the other ~60 families no `ClipRejected` is ever emitted, so an identity obligation over them would
require inventing emit sites the code says do not exist. *Terminal refusal records* — 3 `emitClipReject`
sites plus 2 refusal journal lines. *Correlator call sites* — **40 in product code**, tests excluded,
**including `refused_or` → `await_refusal` (`ui/daw-agent/src/tools.rs:62-67`) with 24 call sites, one
of them `set_clip_text` (`:3239`), a governed command** — a family the predecessor omitted entirely.

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
sites. S3 the thirteen counter-only decisions are extracted and each replaced. S4 the BATCH's two
counter reads resolve to one accessor.

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

**Scope, widened from the predecessor.** The gate covers the whole readiness promise, not the bypass
half alone: per-slot **bypass**, the **parameter mirror replay**, and the **failure of
`sendSetBypass`** itself. The predecessor validated bypass only, which is the review's blocker (4).

**Invariant.** OBSERVABLE: no `ProcessBlock` is dispatched to a host whose per-slot bypass differs
from the authored chain, whose parameter mirror has not been replayed and acknowledged for the same
`host_generation`, or for which a `sendSetBypass` failed without the readiness being withdrawn.
"Authored" means the vector as copied at `apps/daw_engine_main.cpp:1113` under `trackMutex` — that
copy is the only referent that exists, because the guard is released at `:1114` while the `SetBypass`
loop runs at `:1116-1124` under `controllerMutex` alone, so the chain can change across the gap.
MECHANISM: `hostReady` is release-published only after all three are staged and acknowledged.

**Population.** *`hostReady` publish sites* — exactly 4. Command:
`rg -n 'hostReady(\.|->)store\(true' apps`: `engine_restart_worker.cpp:87` (the site under gate),
`engine_track_setup.cpp:62`, and two others. *`controllerMutex` acquisitions in product code* — 28.
Command: `rg -n -g 'apps/**' -g '!apps/*tests_main.cpp' '(lock_guard|unique_lock)<std::mutex>[^;()]*\([^;]*controllerMutex'`.
*`hostReady` read sites in product code* — exactly 20, `daw_engine_main.cpp:1107` among them. Command:
`rg -n -g 'apps/**' -g '!apps/*tests_main.cpp' 'hostReady(\.|->)load' apps`.

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
4. **Mirror replay is staged and acknowledged before publication.** *REFUTED BY* a dispatch observed
   after `hostReady` with the mirror unreplayed for that `host_generation`.
5. **A `sendSetBypass` failure withdraws readiness.** *REFUTED BY* `hostReady` remaining true after a
   forced send failure.
6. Liveness scoped to the path that reaches the publish. *REFUTED BY* the re-run probe being refused
   after `restartInFlight` goes false.
7. The witness set converts a vacuous green into a FAIL: "recovery-hook-entered" appears exactly once
   and the cycle ends with `hostGaveUp` false. *REFUTED BY* a green run with zero hook entries.
8. The control sabotage restores only the ordering and must flip the decider. *REFUTED BY* the decider
   holding under a build whose publish precedes the recovery.

**Static checks.** S1 publish-site ratchet, exactly the recorded 4 lines. S2 single arming site for
the mirror: `rg -n 'mirrorPending(\.|->)store\(true' apps` returns exactly `engine_rt_helpers.cpp:38`.
S3 anti-deadlock: no `mirrorPending|mirrorPrimed|mirrorGateSampleTime` token between
`engine_produce_block.cpp:361` and the lock construction. S4 probe fidelity: `:362` still reads
`hostReady` with acquire and `:385-387` still constructs the blocking lock.

**Review register.** The recovery CONTENT is not mechanically decidable at this SHA: the reviewer
SHALL confirm by reading `apps/daw_engine_main.cpp:1106-1125` that the recovery issues exactly one
`SetBypass` per slot. The reviewer SHALL confirm no artifact describing the fix says "the authored
bypass value" without qualifying it as the copy at `:1113`. The reviewer SHALL confirm, when the fix
lands, that mechanism substitution has not voided the gate. The reviewer SHALL classify any NEW
`hostReady` publish site, which S1 can detect but not judge. **The reviewer SHALL rule on the
self-deadlock**: the fix class this gate admits requires `applyHostBypassStates` to stop taking
`controllerMutex` at `:1116`, i.e. a caller-holds contract or a lock-passing signature — which the
predecessor did not state and which is a design decision, not a detail.

---

# G3 — Per-host failure containment

**Severity** HIGH; blast radius is the entire session. **Dependencies** G2-B **of this packet** — the
gate as received named a document that does not exist in the pinned tree, and the reference is
resolved here rather than left dangling.

**Invariant.** For a NON-MASTER track — a member of the `tracks` vector snapshotted at
`apps/daw_engine_main.cpp:962-969`, which excludes the master constructed at `:449` — a hosted plugin
that stops advancing its `completedBlockId` while its control socket stays writable must, after a
declared bound N counted in OBSERVATIONS OF THAT HOST, have `hostReady` stored false by an autonomous
engine path, and block production for the remaining tracks must continue.

**Population.** *Tracks whose production must continue* — the `tracks` vector, read at
`apps/daw_engine_main.cpp:962-969`. *Writes that remove a host from the gate population* — 17.
Command: `grep -rn 'hostReady\.store(false' apps/`. *Producer-loop exits* — 12 `continue;`. Command:
`sed -n '134,373p' apps/engine_producer_thread.cpp | grep -c 'continue;'`.

**Floor.** The `hostReady` write census is exact at 21 total (4 true / 17 false) with zero non-literal
arguments, and the ratchet also asserts no `exchange`/`compare_exchange` form exists — because a
`store` grep is blind to those. The producer-exit count is a floor of 12: a `return` or a `break` out
of a nested scope is not a `continue`.

**Failure model.** (1) Hung plugin with a writable socket — the case G3 exists for; the host renders
inline on the thread that reads its control socket, so SIGSTOP is a faithful constructor. (2) **The
only per-track detector is unreachable when the gate is shut**: `apps/engine_produce_block.cpp:1096-1101`
clears `hostReady` on `!sentOk`, but is not reached once production is blocked. (3) The producer's
three stall reasons are not three: `logStall` is called with `minCompleted` (`:275`), `inFlight`
(`:285`) and `ahead` (`:328`), but the local `minCompleted` is overwritten between them.

**Deterministic test.** Three layers, no layer's verdict a duration. Layer 0 settles a contradiction
the packet must not carry into a verdict. Layer 1 decides the rule with no process and no clock. Layer
2 decides the rule is reached and the session survives. **The Layer-2 fixture SHALL set
`DAW_ENGINE_DEBUG_STALL`**: every stall line is gated on it (`apps/engine_producer_thread.cpp:62`,
`:80-82`), so without it the channel the PASS bullet reads does not exist — the omission the
predecessor's open list dropped.

**PASS conditions.**
1. Layer 0 decided the experiment, not the engine's lifespan. *REFUTED BY* an M1 whose `kill -0`
   result is taken outside the observation window.
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

**Review register.** The reviewer SHALL obtain an owner ruling for N and record its derivation:
nothing in the tree sources one, all three production `Watchdog`s using `hardTimeoutBlocks = 500`.
The reviewer SHALL rule on which of two contradictory statements is stale after M1 runs, and record
the ruling next to BOTH — `tools/host_stall_check.sh:16-19` and the producer comment. The reviewer
SHALL confirm that evicting a host which still owes dispatched blocks cannot corrupt what a RESUMED
host reads or publishes. The reviewer SHALL decide `daw::Watchdog`'s fate — deletion or
re-specification — since the gate accepts either with equal force and deliberately cannot choose.
**The reviewer SHALL resolve the static-check contradiction**: one check places the eviction inside
`apps/engine_producer_thread.cpp:225-252`, whose natural implementation adds a `continue` and moves
the exit count off 12, which a sibling check forbids. One of the two must yield, and the packet does
not choose.

---

# G4 — Segment and routed-audio ownership

**Severity** HIGH, not CRITICAL, and the reason is load-bearing: the default path is **already**
correctly gated by the mechanism this gate generalises. `apps/engine_audio_callback.h:328/:347` and
`apps/engine_master_render.cpp:80-95` both wait on `completedBlockId`, and the sidechain read at
`apps/engine_produce_block.cpp:907-919` indexes by the COMPLETED id. Those are counter-examples
proving omission rather than design, and they bound the blast radius to three shapes: chains
interleaving VST and non-VST devices, a patcher audio node after a plugin, and track-to-track routing
from a plugin-bearing track. **Dependencies** G0-A, G0-B, G1-A, G1-B, G2-A, G2-B, G3. Final gate.

**Invariant.** For every dispatch, identified by the quadruple (host generation `g`, `blockId b`,
`segmentStart s0`, `segmentLength sl`) — not by the loop ordinal, because segmentation is recomputed
from `trackState.chainDevices` every block (`apps/engine_produce_block.cpp:660-716`) so a chain edit
renumbers ordinals while `(s0,sl)` still names the same plugins — the bytes of that host's INPUT plane
slot for `blockIndex = b % numBlocks` are owned by the host from dispatch until that exact segment
acknowledges consumption. **The full ordering is `write_output → release-ack → acquire-wait →
read_output`**: the host writes its output, releases the acknowledgement with release ordering, and no
consumer reads that output before an acquire-load of that acknowledgement. The predecessor stated
input immutability and ack identity but omitted this ordering, which is what makes the output half
sound rather than merely acknowledged.

**Population.** *Dispatch sites* — 3 production, 8 test, 6 non-calls; command `rg -n sendProcessBlock`
over the pinned root returns 17, every hit classified. *Out-plane readers in engine production code* —
exactly 7; command `rg -n "audioOutOffset|safeAudioOutPtr|audioOutChannelPtr|auxOutputPlaneOffset" apps/`
returns 28, classified. *Input-plane writers in engine production code* — exactly 2; command
`rg -n "audioInOffset|safeAudioInPtr|audioInChannelPtr" apps/` returns 13, all classified.

**Floor.** Dispatch sites floor 3: `rg` finds every syntactic call but is blind to a dispatch through
a function pointer. Reader and writer censuses are floors of 7 and 2 for the same reason plus helper
indirection.

**Failure model.** (1) Input overwritten under the host: the host is delayed before reading segment
k's input; the engine memcpys segment k+1 over it (`:944-1037`). (2) Stale lap read as fresh: the host
is delayed before WRITING segment k's output, and `:1030-1033` reads the audioOut slot for the same
`blockIndex`, which still holds the block from `numBlocks` ago — ~32 ms at 512@48k. (3) Torn input
plane, with no generation or marker to detect it. (4) Routing copies the same un-awaited plane
(`:1147-1154`). The host publishes `completedBlockId` only on the last segment
(`apps/juce_host_process_main.cpp:1069-1087`), so **there is no per-segment signal to wait on even in
principle** — which is why the invariant demands an ack keyed on the quadruple.

**Deterministic test.** Stated first: the device-free offline render (`apps/daw_engine_main.cpp:2143-2153`)
is the byte-exact oracle for CONTENT and is the only driver this gate accepts for content, but its only
barrier is per-block, so ORDERING is decided by a barrier-controlled fake host emitting a merged log.
Chains F0–F6 with distinct per-segment sentinels; schedules S1–S8 hold at every boundary. **The
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
   what the engine's `write_input` recorded for that quadruple. *REFUTED BY* one differing byte.
5. Full-identity comparison, not a field echo: each consumer refuses when ANY ONE of the four
   components is wrong and the other three right. *REFUTED BY* a consumer reading on three of four.
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

**Static checks.** Ack-after-process in the host: the per-segment acknowledgement is stored AFTER the
last `slot.instance->process(...)` (`apps/juce_host_process_main.cpp:987`). Both dispatch branches:
the wait is placed after the join of `if (debugStall) … else …` (`apps/engine_produce_block.cpp:1073-1095`),
not inside either arm. Refuse-on-timeout shape: any deadline at `:1030`, `:1112` or `:1150` places the
READ inside the success branch. The compared value must have a host-side producer — it must be the
word the HOST stores, never `sentOk`.

**Review register.** The reviewer SHALL confirm by reading `apps/juce_host_process_main.cpp:606-1096`
that the SHIPPING host stores the acknowledgement after the last `process`. The reviewer SHALL confirm
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

# Open items — 20, atomic

Listed atomically rather than by category, because the predecessor's 15 categories hid four issues.
None is closed; each names what must be decided or built.

**G0-B** — 1. The generated header breaks the documented `-DDAW_BUILD_PATCHER_RUST=OFF` build for six
unconditional targets, with no stated path, include directory or target-ordering edge. 2. The
declaring macro invalidates the `#[repr(C` grep that this gate's own population floor depends on, so
the gate goes red on its own floor the moment the mechanism lands. 3. **The mutation floor is
unowned**: ">= 600" is asserted with no derivation, and counting the defined kinds over the real 131
members lands near 551, so the battery would fail its own floor permanently. 4. 600 is itself a fifth
pinned integer with no owner named.

**G1-A** — 5. The entry-address extraction returns 21 lines of which 12 are plugin-cache reads; the
ring filter must be in the printed command or the population measures the wrong set. 6. The `ui_out`
producer census is not established, and the disarm is only safe under a single-consumer contract that
is asserted rather than proven.

**G1-B** — 7. The send-site count is stated as 16, as "the sixteen", and as a printed list of 15;
these must reconcile. 8. A region the scope omits is called by the population one of "the only two"
that rewrite in place. 9. Two extraction recipes do not reproduce their own lists. 10. The selection
of 6 readers from 21 is by hand and is the weakest link in this packet's populations.

**G2-A** — 11. Layer-1 fixture arithmetic: 11 journal lines, not 6, and ids legitimately repeat, so a
correct implementation fails the gate's only runnable integration assertion. 12. Twenty-two of the
forty correlator sites read records no bullet requires to carry an id, and sixteen are in a family the
scope excludes; widen the record population or narrow the correlator population, but the packet must
choose. 13. **The BATCH note branch**: `resolve_base` keeps the counter crossing on the path a browser
transpose takes, and the static check as written is satisfied by fixing the chord branch alone.

**G2-B** — 14. **The self-deadlock**: the admitted fix class requires `applyHostBypassStates` to stop
taking `controllerMutex`, i.e. a caller-holds contract — a design decision with no owner ruling. 15.
The swap trap rests on an unratcheted guard at `apps/daw_engine_main.cpp:1107-1109`; either pin it
statically or restate the bullet as a disjunction. 16. **Probe ordering**: without forbidding the
offline probe from acquiring before the RT probe has reported, the PASS token is producible by
contention from the packet's own fixture.

**G3** — 17. A containment bound whose denominator is structurally 1, because the readiness event has
exactly one emission site. 18. **`DAW_ENGINE_DEBUG_STALL` must be set by the Layer-2 fixture**, or the
channel a PASS bullet reads does not exist. 19. **The static-check contradiction**: one check places
the eviction where its natural implementation changes an exit count another check pins. One must
yield; the packet does not choose.

**G4** — 20. Ack-census counts and the master clause needed correcting, and the negative control had
to be respecified against a build that can emit the log it is measured on; both are done here, and the
reviewer should confirm the corrected counts against the fixture rather than against this packet.

# Provenance of this packet's own numbers

Every count was produced by a command run against the pinned read-only checkout, and every count is a
floor where a runtime value defeats the extraction. Counts corrected from the predecessor, in place
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
