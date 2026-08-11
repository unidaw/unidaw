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
2. **The open list is 25 atomic items, not 15 categories.** The four that compression swallowed are
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
- *Regions the engine addresses* — 8, exact. [HAND-CLASSIFIED — open item 25 (all)] Every `ShmHeader` offset field read off a
  controller-derived header plus the two derived functions.
- *Raw region derivations outside any validator* — RAW 13 (`grep -rn -e '>audioInOffset' -e '>audioOutOffset' -e '>ringStdOffset' -e '>ringCtrlOffset' -e '>mailboxOffset' -e auxOutputPlaneOffset -e hostKeyRingOffset apps/ | grep -v _tests_main | grep -v juce_host_process_main | grep -v engine_ui_shm | grep -v audio_shm | grep -v shared_memory | grep -v uiShm`) returns 13 with the exclusions carried INSIDE the
  pipeline (the predecessor said 12, and stated the exclusions in prose beside a command that did
  not apply them).
- *Bounds checks anchored on the child's number* — 7, exact. [HAND-CLASSIFIED — open item 25 (all)]
- *Ring constructions over a host-created mapping* — 3, exact. [HAND-CLASSIFIED — open item 25 (all)]
- **Mailbox `completedBlockId` LOADS — 7 live, 8 syntactic.** Command:
  `grep -rn 'completedBlockId' apps | grep -v -e _tests_main.cpp -e juce_host_process_main.cpp | grep -E '\->load|\.load'`
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

**Population.** *ABI types* — 8. Command: `grep -n '#\[repr(C' patcher_rust/src/lib.rs` returns 8 (lines 23, 40,
55, 68, 82, 89, 97, 107), cross-checked against bindgen's `allowlist_file` closure over
`apps/patcher_abi.h`. *Members* — 66 C++ / 65 Rust, each produced by a command rather than by reading, which is what
open item 5 (G0-B) required. C++: `awk '/^struct (alignas\([0-9]+\) )?(HarmonyEvent|MusicalLogicPayload|PatcherEuclideanConfig|PatcherSliceSelectConfig|PatcherRandomDegreeConfig|PatcherLfoConfig|PatcherContext|EventEntry) \{/{inb=1;next} inb&&/^};/{inb=0;next} inb{l=$0;sub(/\/\/.*/,"",l); if(l~/;/ && l!~/static_assert|static constexpr|typedef|using |\(/) c++} END{print c}' apps/harmony_timeline.h apps/patcher_abi.h apps/shared_memory.h` returns 66. Rust: `awk '/^#\[repr\(C/{r=1;next} r&&/^pub struct/{inb=1;r=0;next} inb&&/^}/{inb=0;next} inb&&/^[ \t]+(pub )?[A-Za-z_][A-Za-z0-9_]*[ \t]*:/{c++} END{print c}' patcher_rust/src/lib.rs` returns 65. The PER-TYPE breakdown is commanded too, because a total that
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
not have reproduced this. *One-sided members* — exactly 1 [HAND-CLASSIFIED — open item 25 (all)], corroborated independently by the per-type block above, whose only row with unequal sides is `EventEntry 7/6`: C++ `EventEntry::ready`, offset 60, size 4
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
   lands — the rename itself is open item 24 (G0-B), because a gate that declares itself RED and
   registers no work to clear it reads as a permanent condition rather than a task. *REFUTED BY* the
   check exiting 0 with `patcher_abi.h:75 reserved` and `lib.rs:86 _pad0` both unchanged.

**Static checks.** S-1 the generated table's include sits above the first export at `patcher_rust/src/lib.rs:5` (`pub const PATCHER_ABI_VERSION`); the predecessor anchored this to `lib.rs:893`, which is the `#[cfg(test)]` line 892 lines BELOW that export, so the check it stated would have passed for an include placed under every export in the file. A
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

**Population.** Every count is stated as RAW (what the command returns) → RULE (the classification)
→ IN SCOPE, because a bare figure that is a hand-classified subset of its own command is not
reproducible, and four populations in the predecessor were exactly that.
- *Rings* — RAW 17 (`grep -rnF '>capacity' apps/ | grep '=' | grep -v '=='`) → minus 9 non-ring and
  test-fixture assignments → **8**.
- *Statements interpreting an entry's bytes as data* — RAW 21 (`grep -rnF 'entries[' apps/`)
  → minus 17 (twelve plugin-cache index sites, which are a DIFFERENT `entries` array, and five ready-flag
  operations, which touch a synchronisation field rather than the entry's data) → **4**:
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
  grep — and the rule now also ships INSIDE a command, which is what open item 6 (G1-A) asked for:
  `grep -rnF 'entries[' apps/ | grep -v -e pluginCache -e cache.entries | grep -v -e storeReady -e loadReady`
  returns 4, and appending `| grep -v _tests_main` returns 3. A rule stated beside a command is a
  claim about a classification; a rule stated inside one is the classification.
  The ring filter is in the rule, not implied: without it this population measures the wrong set.
- *Read-cursor stores* — RAW **14** (`grep -rn -e readIndex.store -e read_index.store apps/ ui/`) → minus 10 non-ring and test stores → **4**. The predecessor printed this command beside the figure 4, which
  the command does not produce.
- *Index sites* — RAW 21 (`grep -rnF 'entries[' apps/`) → minus 9 event-ring statements → **12**,
  being exactly the plugin-cache reads subtracted above: `plugin_cache.cpp:388/:460/:469/:478/:492`,
  `engine_chain_commands.cpp:81`, `engine_save_project.cpp:265/:362`, `daw_engine_main.cpp:291/:1078`,
  and two in `_tests_main` files. The two populations partition the same 21 and are stated so the
  partition is visible: 12 + 9 = 21, and the 9 split 4 data / 5 flag. Inside a command:
  `grep -rnF 'entries[' apps/ | grep -e pluginCache -e cache.entries` returns 12.

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
   `ready` load, on a ring with six live producers — is closed. *REFUTED BY* that site still reading
   `entry.type` with no preceding acquire load of that slot's `ready`, which the S2 predicate decides
   on the source; a runtime observation cannot, because an unpublished slot's bytes are
   indistinguishable from a published slot's when the producer has not yet written.

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

**Population.** *Seqlock opens and closes* — 4 + 4. Command: `grep -rn 'seq\.store' apps` returns 8, cross-checked per file with grep's count mode. *Request/answer readers* — the command `grep -rn 'pub fn read_' ui/daw-bridge/src/control.rs` returns 21 candidates; this gate declares NO population, so no RAW claim is made and none may be inferred. RULE: **NONE THAT WORKS — WITHDRAWN.** The
predecessor selected six BY HAND, which made the number honest and the population irreproducible. This
packet proposed a predicate — intersect the reader's region symbol with the regions an engine REQUEST
handler writes — and it is WRONG: `grep -rn 'Region\*' apps/engine_request_commands.cpp` returns 2, two
region types (`UiDeviceParamsRegion`, `UiWaveformRegion`), so the intersection produces **2**, not 6,
and the FAIL clause that predicate shipped with fires at the SHA it was published at. It is withdrawn
rather than replaced by a third guess. **This gate has no derivable population and cannot be decided
until one exists** — open item 11 (G1-B), and it blocks G1-B the way the missing N blocks G3. *Call sites of
those six* — **WITHDRAWN, inheriting the withdrawal above.** The figure was 16 production and 2 test,
obtained per name and discarding definitions and three doc comments, and the arithmetic is sound;
what it counts is call sites OF A POPULATION THAT NO LONGER STANDS. A count over a withdrawn set is
not a smaller claim than the set — it is the same claim wearing a number, so withdrawing the six and
keeping their call-site census would have left the hand selection in the packet under a different
name. It returns with the population, under open item 11 (G1-B). Open items 8, 9 and 10 all close on
this one withdrawal, which is the evidence that they were symptoms: three separately-filed defects
— an inconsistent count, a scope/population contradiction, and two recipes that did not reproduce
their lists — were one hand-selected population being described three times.

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
   universal form ("no reader ...") is deliberately NOT used: it quantifies over the withdrawn
   population and would be undecidable for the same reason the population is. Both named sites fail today
   and the bullet names both: `ui/daw-bridge/src/control.rs:944-949`. Quantified over THOSE TWO
   SITES, not over "the six" — the six are withdrawn, and a bullet phrased "two of the six" would
   have been undecidable for the same reason the population is. Whether two is the whole set of
   offending sites is open item 11 (G1-B); that these two offend is decidable at this SHA and is
   what the bullet asserts. *REFUTED BY* either remaining.
8. An abandoned publication is distinguishable from an unanswered one within a bounded number of
   observations, and the reader reports which it saw. *REFUTED BY* a reader that returns the same
   value for both. This replaces a predecessor bullet requiring only that the condition be
   "observable", which any implementation satisfies.
9. **WITHDRAWN WITH THE POPULATION.** The bullet required a mechanical echo ratchet over "the send
   sites" and a ratchet needs a set to range over; this packet withdraws that set, so the bullet
   ranged over nothing. It is withdrawn for the same reason PASS 7 was re-quantified rather than
   deleted — the requirement is real and returns with open item 11 (G1-B), which must deliver a
   population before any ratchet over it can be specified. Leaving it standing would have been a
   PASS condition satisfiable by ratcheting the empty set.

**Static checks.** S1 fence between open and first payload store — statement ORDER is invisible from
any return value, since a clean image proves nothing about the fence. S2 write order inside the
device-params body: identity words first (`:122-123`), `paramCount` last (`:169`), which is what makes
the stale answer readable as complete. S3 minting expressions, naming the two collapsing forms. S4
**WITHDRAWN WITH THE POPULATION** — it required drift detectors asserting "the three counts", which
were counts over the hand-selected six this packet withdraws. A drift detector over a withdrawn
population detects drift in nothing. It returns with open item 11 (G1-B), and the requirement it
encodes — that whatever population replaces the six must have detectors pinning its size — is
carried there rather than left as an unrooted static check here.

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
`SetAutomationTarget`, `SetClipText`, `WriteHarmony`, `DeleteHarmony`. RAW 17
(`grep -rn -e 'requireMatchingClipVersion(' -e 'requireMatchingHarmonyVersion(' apps | grep -v tests_main`)
→ minus 8 (three definitions, three declarations, and the two `daw_engine_main.cpp` lambda forwarders
at `:1739` and `:1744`) → **9 call sites**. The predecessor described this command in prose instead of
printing one, so its 9 could not be re-run. The nine COMMANDS map onto the nine sites non-bijectively
— the chord site carries both `WriteChord` and `DeleteChord` through a runtime-computed type — so the
correspondence is established by the Floor paragraph below, not by this arithmetic.
`commandMutatesDocument` (`apps/engine_command_mutates.h:46-236`) has **93** case arms, 69 returning
true — the predecessor said 186, off by 2× in the section whose credibility rests on counts — but for
the other ~60 families no `ClipRejected` is ever emitted, so an identity obligation over them would
require inventing emit sites the code says do not exist. *Terminal refusal records* — 3 `emitClipReject` sites
(`git grep -n 'emitClipReject(' -- apps | grep -v -e 'void emitClipReject' -e daw_engine_main.cpp -e _tests_main`
returns 3) plus 2 refusal journal lines. *Correlator call sites* — RAW 56 mentions
(`grep -rn -e await_clip_outcome -e report_outcome_from -e report_refusal_outcome -e refused_or ui/daw-cli/src ui/daw-agent/src`)
→ minus 5 (four `fn` definitions, and the doc comment at `ui/daw-cli/src/main.rs:1262` that names
`await_clip_outcome` in prose) → **51 call sites**, which is 4 + 6 + 17 + 24 exactly as the exact
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

**Scope, widened from the predecessor.** The gate reaches per-slot **bypass** and the **failure of `sendSetBypass`**.
It does NOT currently reach the **parameter mirror replay**: the bullet that did was withdrawn as
circular (see PASS 4), so the mirror half is specified by the register and open item 18 (G2-B), not by a PASS
condition. The predecessor asserted the mirror half was in scope while its bullet was unsatisfiable, which is
worse than the gap it was closing.

**Invariant, stated in full, with the clause this gate cannot decide marked as such.** OBSERVABLE:
no `ProcessBlock` is dispatched to a host whose per-slot bypass differs from the authored chain,
**[NOT DECIDED HERE — open item 18 (G2-B)]** whose parameter mirror has not been replayed and
acknowledged for the same `host_generation`, or for which a `sendSetBypass` failed without the
readiness being withdrawn. The middle clause is a property of the SYSTEM and belongs in the
invariant; it is not a property this gate can decide at this SHA, because the acknowledgement it
would test for arrives only during a `ProcessBlock` that `processTrack` refuses while `hostReady` is
false. Scope excluding the mirror while the invariant required it was a real contradiction and is
resolved by marking the clause rather than by deleting it — deleting it would make the gate
self-consistent by forgetting the requirement, which is how the predecessor's version came to assert
the mirror half was in scope with an unsatisfiable bullet behind it.
"Authored" means the vector as copied at `apps/daw_engine_main.cpp:1113` under `trackMutex` — that
copy is the only referent that exists, because the guard is released at `:1114` while the `SetBypass`
loop runs at `:1116-1124` under `controllerMutex` alone, so the chain can change across the gap.
MECHANISM: `hostReady` is release-published only after all three are staged and acknowledged.

**Population.** *`hostReady` publish sites* — RAW 4 (`grep -rn 'hostReady' apps | grep 'store(true'`) → minus 1 in `_tests_main` → **3 production**: `engine_restart_worker.cpp:87` (the site under gate),
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
   bullet replaces it until the owner rules on a recovery-only priming protocol; a PASS condition
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
`hostReady` publish site, which S1 can detect but not judge. **The reviewer SHALL rule on the mirror-ack circularity**: the ack arrives only during a
`ProcessBlock`, which `processTrack` refuses while `hostReady` is false. Either recovery gets a
priming path that dispatches under an explicit recovery-only exemption, or readiness is staged in two
levels (mapped-and-bypassed versus mirror-complete) with dispatch permitted at the lower one. Both are
design decisions with consequences for G4's dispatch identity, and this packet deliberately does not
choose. **The reviewer SHALL also rule on the self-deadlock**: the fix class this gate admits requires `applyHostBypassStates` to stop taking
`controllerMutex` at `:1116`, i.e. a caller-holds contract or a lock-passing signature — which the
predecessor did not state and which is a design decision, not a detail.

---

# G3 — Per-host failure containment

**Severity** HIGH; blast radius is the entire session. **STATUS: NOT DECIDABLE AT THIS SHA — BLOCKED
ON AN OWNER RULING.** Every PASS condition below is written against a bound N counted in observations,
and nothing in the tree sources N: all three production `Watchdog`s use `hardTimeoutBlocks = 500`,
which is a block count and not an observation count, and no other artifact proposes one. The exact
review's verdict that this gate "remains undecidable" is accepted rather than argued. The bullets are
retained because they are correct GIVEN N, and they become decidable the moment N is ruled — but no
implementation may be accepted against this gate until then, and the packet does not pretend
otherwise. **Dependencies** an owner ruling on N, then G2-B **of this packet** — the
gate as received named a document that does not exist in the pinned tree, and the reference is
resolved here rather than left dangling.

**Invariant.** For a NON-MASTER track — a member of the `tracks` vector snapshotted at
`apps/daw_engine_main.cpp:962-969`, which excludes the master constructed at `:449` — a hosted plugin
that stops advancing its `completedBlockId` while its control socket stays writable must, after a
declared bound N counted in OBSERVATIONS OF THAT HOST, have `hostReady` stored false by an autonomous
engine path, and block production for the remaining tracks must continue.

**Population.** *Tracks whose production must continue* — [HAND-CLASSIFIED — open item 25 (all)] the `tracks` vector, read at
`apps/daw_engine_main.cpp:962-969`. *Writes that remove a host from the gate population* — RAW 17
(`grep -rn 'hostReady' apps/ | grep 'store(false'`) → minus **2** in `_tests_main` → **15 production**. The
whole census is RAW 21 (`grep -rn 'hostReady' apps/ | grep 'store('`) → minus 3 in `_tests_main` → **18 production**. The predecessor subtracted
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
from a plugin-bearing track. **Dependencies** G0-A, G0-B, G1-A, G1-B, G2-A, G2-B, G3. Final gate — and therefore NOT DECIDABLE
at this SHA, because FOUR of its dependencies carry BLOCKING items: 11 (G1-B) has no reader
population, 18 (G2-B) has the mirror-ack circularity, 19 (G3) has no source for N, and 24 (G0-B)
holds the rename PASS 9 is RED without — G4 depends on G0-B, so a count of three was wrong the
moment 24 was registered. G4 is written
in full so the work is specified, but no PASS verdict on it may be recorded until those FOUR are
ruled; a final gate that reports green over blocked dependencies is the exact shape of a check that
passes with the defect present.

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

**Population.** *Dispatch sites* — RAW 17 (`git grep -n sendProcessBlock`) → minus 14 (8 in test
mains, 6 non-calls: a design doc, a tools script, a log line, a declaration, a comment and the
definition) → **3 production**. The predecessor stated the split beside the command instead of as a
subtraction, which put it outside the arithmetic the gate checks. *Out-plane readers* — RAW **27**
(`grep -rn -e audioOutOffset -e safeAudioOutPtr -e audioOutChannelPtr -e auxOutputPlaneOffset apps/`)
→ minus 20 (three in `_tests_main`, the remainder declarations and non-reads) → **7 production**. The
predecessor said the command returns 28; it returns 27, and the reviewer's count is the correct one. *Input-plane writers in engine production code* — RAW 13
(`grep -rn -e audioInOffset -e safeAudioInPtr -e audioInChannelPtr apps/`) → minus 11 (tests,
declarations and reads) → **2**.

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
11. **Output equality in the BYTES — the mirror of 4, and the bullet whose absence let a stale-slot
    read satisfy this whole gate.** Every consumer's BEFORE_OUTPUT_READ snapshot equals what the
    host's `write_output` recorded for the SAME quadruple, and differs from the lap primed by 8.
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

# A.0 — the gate this packet is decided by

`tools/p12_selfcheck.py` at this SHA, PREV PACKET BLOB `de5917d0a3ece156ea47a6af8702ee97f1c9605a`, A.0 SCRIPT BLOB `09ff586c22fd7204d9be32feef9f86fb306c3a23`
— the script hashes itself and refuses if the packet pins a different blob, so the gate and the
document it decides cannot move apart. It refuses to run unbound: `AE_P12_PIN` must name a checkout of
product `75c6f064` whose tree is `699abfe8` with zero modified paths, and the packet file must equal
its committed blob unless `AE_P12_DRAFT=1` is set for an unpublishable draft run. Every refusal exits
**2**, so a broken gate can never be read as a passing one. Invocation and expected output:

    AE_P12_PIN=<pin> python3 tools/p12_selfcheck.py
    packet blob <oid> · product 75c6f064 tree 699abfe8 · 25 items, 19 open · 13 RAW (12 hand-ruled) + 20 commanded claims, all executed
    PASS

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
deserved excluding. Whether a population's predicate is the right one: three figures in this packet
were re-derived against a predicate of the author's that was wrong (mentions for call sites, `apps/`
for the whole root, a forwarding lambda for an emit site), and in all three the packet was right and
the recheck was wrong. Whether this commit is the intended one. The binding pins the PARENT commit and the parent's
packet blob, so the chain is immutable link by link — but two sibling children of the same parent
are indistinguishable from inside either of them, because no commit can contain its own hash. The
tip SHA and packet blob in the announcement are the external pin; a reviewer must take them from
there, not from the document. Whether a rename is the right resolution for open item 24 (G0-B) — the gate can see the two
names differ and cannot choose which side moves.
And nothing about the product beyond what a text search can see.

**Controls.** Twenty-three, each naming the tag it must provoke; a control that mutates the file without
provoking its own tag reports `BLIND` and fails. The prose count and the names are themselves
checked against the harness, because this list said thirteen for two SHAs after the harness had
eighteen. Run them with `--negative <name>`, list with `--list`: `closed-count`, `dangling-ref`,
`drop-refutation`, `member-dropped`, `member-per-type`, `open-arithmetic`, `open-count`,
`orphan-number`, `raw-without-cmd`, `rg-command`, `rule-arithmetic`, `stale-a0-sample`,
`byhand-count`, `control-unlisted`, `handmade-count`, `root-wide-grep`, `ungated-ref`, `unmarked-popn`,
`unstated-return`, `withdrawn-claim`, `wrong-command`, `wrong-gate-ref`, `wrong-raw`. Two of them were themselves defective when first
written and are recorded here rather than quietly fixed: `raw-without-cmd` changed a claim's NUMBER
and so provoked a different check entirely, and the landing assertion demanded the anchor count DROP,
which an insertion control can never do — it reported a landed mutation as unlanded. The named-tag
requirement is what exposed both.

# Owner rulings (claude-worker-2 as ADR/phase owner, at this SHA)

Five items were carried as "the owner SHALL rule". I am that owner; carrying them further would be
deferral wearing the costume of rigour. Each ruling states what it decides, why, and what it costs,
and each is a DECISION — none of them is evidence, and none may be cited as though a command
produced it.

**R1 — item 11 (G1-B) and item 25 (all): AUTHORED POPULATIONS, with drift detectors.** No predicate
distinguishes a request/answer reader from any other `read_` function, and none distinguishes a
REGION from the offset fields addressing it. Three attempts at a predicate produced 6 by hand, then
2 by intersection, then a withdrawal. The ruling is that these populations are **authored, not
derived**: the packet lists the members explicitly and says so, and each authored list carries a
DRIFT DETECTOR — a command whose figure is pinned, so that a change in the surrounding code
invalidates the authored list instead of silently outdating it. For G1-B that detector is
`grep -rn 'pub fn read_' ui/daw-bridge/src/control.rs` returns 21; if it ever returns anything else,
the authored list is stale by construction and the gate fails until it is re-authored.
This is weaker than a derivation and stronger than what preceded it, which was a hand selection
presented as a population. It also matches what the packet already does for the five
hand-classified populations, so the two items resolve on one mechanism rather than two.
**Cost:** a reviewer must read the list, not re-run a command. The detector bounds the staleness,
not the correctness.

**R2 — item 18 (G2-B): TWO-LEVEL READINESS, not a recovery-only exemption.** Readiness is staged —
`mapped-and-bypassed` and `mirror-complete` — and dispatch is permitted at the lower level. The
exemption was the smaller change and is the worse one: it creates a dispatch path that exists only
during recovery, which is the least-exercised state in the system and the one that runs when
something has already gone wrong. A LEVEL is a value that every dispatch can carry and every
assertion can read; an EXEMPTION is a rule that lives in whichever branch remembered it, which is
this repo's documented failure shape. **Cost, stated because it is real:** G4's dispatch identity
must carry the readiness level, so the quadruple becomes a quintuple or the acknowledgement must
name the level it was minted under. G4's PASS 5 full-identity comparison changes with it. This
ruling therefore reaches into G4 and is not free.

**R3 — item 19 (G3): N IS AUTHORED AT 3, and the packet says authored, not derived.** Nothing in
the tree sources an observation count; `hardTimeoutBlocks = 500` is a block count and converting it
would be inventing a rate. So N is a design constant: **a host must fail to advance across 3
consecutive observations before eviction, and 3N = 9 observations of absence**. The constraint the
value must satisfy is stated so a successor can rule differently on evidence: N observations must
be long enough to survive ordinary scheduling jitter and strictly shorter in wall-clock than the
existing hard timeout, so containment happens before the blunt instrument fires. A static check
pins the literal, so changing N is a visible edit rather than a drift. **Cost:** 3 is not measured.
It is the smallest value that is not 1 — 1 evicts on a single unlucky sweep — and it is offered as
a starting point that measurement should overturn.

**R4 — item 24 (G0-B): THE RUST SIDE RENAMES.** `patcher_abi.h:75` keeps `reserved`;
`patcher_rust/src/lib.rs:86` changes `_pad0` to `reserved`. The C++ header is the ABI AUTHORITY —
bindgen consumes it through an `allowlist_file` closure — and renaming the authority to match its
own generated mirror inverts which document defines the contract. The convention argument cuts the
other way (`_pad0` is idiomatic Rust) and loses to that: a binding may be unidiomatic, an authority
may not be derived from its binding. **Cost:** one unidiomatic name in the Rust file, and PASS 9
goes from RED to decidable the moment the rename lands.

**What these rulings do NOT do.** None of them closes its item — R1 through R4 are decisions that
make the items IMPLEMENTABLE, and each item stays open until the work it names exists and is
verified by someone other than me. A ruling recorded as a closure would be the same error as a
census recorded as a proof, which this packet has already made once at item 7.

# Open items — 25 atomic, 6 CLOSED at this SHA, 19 open

One per line, numbered in document order, so the count is checkable. FOUR are BLOCKING — 11, 18, 19 and 24, matching the four G4 names as its undecidability: a gate
carrying one cannot be decided by any implementation.

1. **G0-B** — The generated header breaks the documented `-DDAW_BUILD_PATCHER_RUST=OFF` build for six unconditional targets, with no stated path, include directory or target-ordering edge.
2. **G0-B** — The declaring macro invalidates the `#[repr(C` grep this gate's population floor depends on.
3. **G0-B** — The mutation floor ">= 600" is asserted with no derivation and counts near 551 over the real 131 members, so the battery would fail its own floor permanently.
4. **G0-B** — 600 is a fifth pinned integer with no owner.
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
    has a whole second reader I did not look for: `peek_ui_diffs` (`control.rs:444`) with 7 call
    sites in `ui/daw-cli/src/main.rs`. My claim of "one drain function, one caller, no second
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
   7 is re-quantified over the two sites it names. Both return under open item 11 (G1-B).
9. **G1-B** — CLOSED at this SHA as a defect, SUBSUMED by open item 11 (G1-B) as subject matter.
   The contradiction was between a scope that omitted a region and a population that called that
   region one of "the only two" rewriting in place. The population making that claim is withdrawn,
   so the contradiction has no second side; the phrase appears nowhere in this packet. It returns
   the moment a derivable population does, and it returns as a REQUIREMENT on that population — any
   successor's population must be checked against the scope before it is published, which is the
   check whose absence produced this item.
10. **G1-B** — CLOSED at this SHA as a defect, SUBSUMED by open item 11 (G1-B). Both recipes now
    state what they actually return rather than a list they do not produce: `pub fn read_` returns
    21 candidates and is explicitly NOT a population, and the `Region*` intersection returns 2 and
    is published as WRONG with the FAIL clause it shipped with. A recipe that names its true output
    cannot fail to reproduce its list, because it no longer claims one.
11. **G1-B** — **BLOCKING, RULED (R1), not closed.** The reader population has no derivation. The hand selection of six was irreproducible; the predicate proposed to replace it yields two, not six, and is withdrawn. This gate cannot be decided until a population exists, as G3 cannot until N exists.
12. **G2-A** — Layer-1 fixture arithmetic: eleven journal lines, not six, and ids legitimately repeat, so a correct implementation fails the gate's only runnable integration assertion.
13. **G2-A** — CLOSED at this SHA. The fifty-one correlator sites are no longer carried on attribution: the G2-A population states the command, its raw count and the subtraction, and reaches the exact review's 4 + 6 + 17 + 24. The count is written once, there, so this entry does not restate it. Re-deriving it is also what refuted my own competing figure — that one counted mentions rather than call sites, so the disagreement was my predicate and not the reviewer's arithmetic.
14. **G2-A** — The BATCH note branch: `resolve_base` keeps the counter crossing on the path a browser transpose takes, and the static check as written is satisfied by fixing the chord branch alone.
15. **G2-B** — The self-deadlock: the admitted fix class requires `applyHostBypassStates` to stop taking `controllerMutex`.
16. **G2-B** — The swap trap rests on an unratcheted guard at `apps/daw_engine_main.cpp:1107-1109`.
17. **G2-B** — Probe ordering: without forbidding the offline probe from acquiring before the RT probe reports, the PASS token is producible by the packet's own fixture.
18. **G2-B** — **BLOCKING, RULED (R2), not closed.** The mirror-ack circularity. The ack arrives only during a `ProcessBlock` that `processTrack` refuses while `hostReady` is false. A PASS bullet was withdrawn rather than reworded, and the mirror half is unspecified until the owner rules between a recovery-only priming exemption and a two-level readiness.
19. **G3** — **BLOCKING, RULED (R3: N = 3, authored), not closed.** N has no source in the tree, so this gate is NOT DECIDABLE and no implementation may be accepted against it until an owner rules.
20. **G3** — `DAW_ENGINE_DEBUG_STALL` must be set by the Layer-2 fixture or the channel a PASS bullet reads does not exist.
21. **G3** — The static-check contradiction: one check places the eviction where its natural implementation changes an exit count another check pins.
22. **G4** — The fixture definitions added here (F0-F6, S1-S8) and the corrected ack-census counts have not been run against anything; the reviewer should confirm them against the fixture rather than against this packet.
23. **all** — `read_clip_window` is named by the exact review as a request/answer reader G1-B omits. It cannot be placed until item 11 gives the gate a population.

24. **G0-B** — BLOCKING for G0-B only. **RULED (R4): the Rust side renames.** The pure name join requires one rename: `patcher_abi.h:75`
    declares `uint8_t reserved[4]{}` where `patcher_rust/src/lib.rs:86` declares `pub _pad0: [u8; 4]`,
    so the two sides of a byte-identical member disagree by NAME and no rule that forbids an alias
    table can join them. PASS 9 is RED until this lands and states so. Which side renames is an owner
    call, not a derivation: `reserved` is the C++ convention used elsewhere in that header and `_pad0`
    is the Rust convention, so the choice trades one file's internal consistency against the other's.
25. **all** — **RULED (R1): authored with drift detectors.** The five HAND-CLASSIFIED populations, tracked rather than claimed away. `Regions the
    engine addresses` (8), `Bounds checks anchored on the child's number` (7), `Ring constructions
    over a host-created mapping` (3), `One-sided members` (1) and `Tracks whose production must
    continue` are semantic groupings, not text matches: no grep distinguishes a REGION from the
    offset fields that address it, and counting `Offset` declarations in `apps/shared_memory.h`
    returns 27 against a claim of 8, which is the distance between the two ideas. Each carries the
    marker and this item; a sixth appearing without one fails the gate. Deriving them needs a
    predicate nobody has proposed, so this is open, not closed.

# Provenance of this packet's own numbers

Every count is stated as RAW → RULE → IN SCOPE, so that the command reproduces the raw figure and the
rule reproduces the rest; and every count is a floor where a runtime value defeats the extraction.
**5 populations are HAND-CLASSIFIED and exempt from that sentence**, each carrying a marker and
open item 25 (all). **And of the 13 RAW claims, 12 of them apply their RULE BY HAND** — the command
returns the raw figure and a stated subtraction reaches the in-scope one — while 1 carries the rule
inside the command and needs no subtraction at all. That 12 is the honest size of what this gate
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
