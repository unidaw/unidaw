# P2-CMD-00 (revised) — one identity for every refusal, at one offset

**State** DESIGN, read-only. No product or wire change is made by this document.
**Author** claude-worker-1 · 2026-08-13
**Supersedes** `P2-CMD-00-command-outcome.md`, whose measurements were wrong in three places.
**Against** current files at d6364f7c. Every number below was produced by a compiled program.

## What the review changed

`P2-CMD-00-review.md` found the original wrong on scope (2 of 6 payloads), capacity (16 bytes, really
8), the `<=`→`==` justification, and the minting rule. Revising against those findings surfaced two
more facts that neither the design nor its review had, and both change the shape again.

## 1. Scope — seven payloads, not two, and one offset serves them all

Six carry a refusal; `UiHarmonyDiffPayload` carries the harmony channel's outcome and is the seventh
that needs the field. Measured `offsetof`/`sizeof`/`alignof`:

| payload | size | align | reserved run starts | bytes 32–40 free |
|---|---|---|---|---|
| `UiClipRejectPayload` | 40 | 4 | 18 | ✅ |
| `UiHarmonyDiffPayload` | 40 | 4 | 24 | ✅ |
| `UiSamplerRejectPayload` | 40 | 4 | 16 | ✅ |
| `UiChainErrorPayload` | 40 | 4 | 20 | ✅ |
| `UiRoutingErrorPayload` | 40 | 4 | 8 | ✅ |
| `UiModErrorPayload` | 40 | 4 | 12 | ✅ |
| `UiPatcherGraphErrorPayload` | 40 | 4 | **32** | ✅ (exactly) |

Every reserved run reaches byte 40, so **bytes 32–40 are free in all seven**. That gives a result the
original could not have had from two samples: **one uniform offset, 32**, so a reader that has already
established the entry *is* a refusal does not then need to know **which** of the seven it is in order
to find the id. `UiPatcherGraphErrorPayload` is the binding constraint — its reserved run is exactly
those 8 bytes — and it is the payload that forces the answer rather than one that barely accommodates
it.

**This does not remove the dispatch.** §6 measures a publisher that emits uninitialised payload bytes,
so a reader must still confirm the entry's `type` is a refusal and that `size` covers offset 40 before
reading there. One offset saves the reader a *second* dispatch over payload shapes, not the first one
over event type. My first draft of this paragraph said "without dispatching on payload type first" and
contradicted §6 two screens below — the same superseded-rule-stated-elsewhere defect this project has
now paid for repeatedly, committed inside the document that measures it.

## 2. The id is two `uint32_t`, not a `uint64_t` — and this is not cosmetic

Backend's instruction says "u64-only capacity". The **capacity** is 8 bytes and that is right. The
**representation** cannot be a `uint64_t` member, and the reason is not style:

```
EventEntry: size=64 align=64, payload at offset 20
```

`payload` sits at offset **20**, which is 4-aligned and **not** 8-aligned. All seven payloads are
`alignof == 4` today, so that is consistent, and the codebase casts payload bytes to payload structs —
e.g. `reinterpret_cast<const daw::HelloRequest*>(payload.data())` at
`apps/juce_host_process_main.cpp:1188`, and three more at `:1196`, `:1204`, `:1212`.

Adding a `uint64_t` member raises the struct's alignment to 8 (verified: a mock with the field is
`size=40 align=8`). A struct requiring 8-byte alignment can never be legally cast at `entry+20`.
`sizeof` stays 40 and every `memcpy` reader keeps working, so **the only symptom is undefined
behaviour at the cast sites** — silent on x86, a fault on a strict-alignment target, and invisible to
every size assertion the original design proposed.

Two `uint32_t` halves carry the same 8 bytes at the same offsets and keep `alignof` at 4 (verified:
`size=40 align=4, lo@32 hi@36`):

```
uint32_t correlationLo;   // offset 32 in every refusal payload
uint32_t correlationHi;   // offset 36
```

The Rust mirror already reads payload bytes as `u32::from_le_bytes([payload[o], …])`, so this costs
nothing there. **The alignment invariant to state in the header is: no payload struct may exceed
`alignof == 4`, because `EventEntry::payload` is at offset 20.** That invariant is currently true by
accident and nothing checks it.

## 3. Minting — the single-producer premise is stale, and no registry is needed

The original proposed "a monotonic per-client counter with the client's identity in the high bits".
The review found no client identity exists. The deeper problem is that the premise everyone was
reasoning from is out of date:

- `ui/daw-bridge/src/control.rs:4-7` states *"the UI command ring is SPSC … exactly one process may be
  the producer."*
- `apps/shared_memory.h:440-446` describes `EventEntry::ready`, the **multi-producer** publication
  flag added in M2.18: a producer CAS-reserves a slot, fills it, then stores `ready=1` — *"which is
  why the ring **was** single-producer and `daw-cli do` needed `--force`."*

Past tense. **The rings are multi-producer; the Rust doc comment describes the world before M2.18** and
is a documentation defect in its own right (reported separately; not fixed here, this is read-only).

So the id must be unique across **concurrent producers** *and* across **producer lifetimes** — a
restart resets a counter while the previous producer's refusals are still visible, because the ring is
peeked rather than drained.

**Proposal: a per-process nonce, drawn once at attach.**

```
correlationHi = a 32-bit random nonce, drawn once per producer process when it attaches
correlationLo = a monotonic counter within that process, starting at 1
```

- needs **no** SHM field, no registry, and no allocation authority — which is what killed the original
  scheme, since `UiCommandType` allocation already requires bus coordination;
- unique across concurrent producers and across restarts by the same argument;
- collision is birthday-on-2³². At 100 producer lifetimes in a session the probability is ~1×10⁻⁶; at
  1000 it is ~1×10⁻⁴. **State this bound in the header** rather than calling it unique.

*The exact alternative*, stated so the choice is deliberate: a producer-id claim array in the SHM with
CAS acquisition gives exactness instead of a probability, at the cost of a wire change, an allocation
authority, and a stale-claim reclamation rule. **The nonce is the recommendation**; the exact scheme is
the fallback if anyone objects to a probabilistic identity in a correctness path. This is an owner
decision I have not made.

## 4. What the engine does with it: nothing

Echo verbatim. Any interpretation makes it a second version counter and reintroduces the arbitration
this sits beside. Unchanged from the original, and it is the part that was right.

## 5. Offset assertions, because size assertions cannot see this

The original proposed tightening `sizeof <= 40` to `== 40`. The review showed that is justified by a
false claim — the struct is already exactly 40, so `<=` *does* catch an added field — and that neither
form detects the mechanism this design uses: **carving a meaning out of `reserved` changes no size.**

`apps/event_payloads.h` has exactly **two** `static_assert(offsetof(...))` today, both
`diffType == 0`. The convention exists and is thin. Required per payload:

```
static_assert(offsetof(T, correlationLo) == 32, "...");
static_assert(offsetof(T, correlationHi) == 36, "...");
static_assert(alignof(T) == 4, "EventEntry::payload is at offset 20; an 8-aligned payload cannot be cast there");
static_assert(sizeof(T) == 40, "...");
```

Four per payload × seven payloads = 28 assertions, plus the same offsets asserted on the Rust side
where the mirror check already lives. The `alignof` one is the assertion that would have caught the
mistake in §2, and no assertion in the tree today would have.

## 6. Migration gates

`kShmVersion` is **37** (`apps/shared_memory.h:141`), mirrored as `K_SHM_VERSION = 37` in
`ui/daw-bridge/src/layout.rs:7`. Both move in one changeset with `SHM_LAYOUT.md`.

- **one coordinated bump to 38**, carrying all seven payload changes at once. Giving a reserved field a
  meaning *is* a wire change even though no size moves — that is precisely how a wire change lands
  without a bump.
- **`correlationLo == 0 && correlationHi == 0` means "no id"**: a sender that predates 38. A reader
  must report `Unknown`, never a match.

  **Measured, since this was the one input left open.** The sentinel is safe *for refusal payloads*:
  all nine emit sites construct the struct `{}`-initialised (`engine_ui_publish.cpp:237, 280, 359,
  396, 423, 442` and `engine_harmony_timeline.cpp:58, 102, 118`), every one of the seven is exactly
  40 bytes so the `memcpy` covers the whole `payload[40]`, and `ringWrite` assigns the **whole**
  `EventEntry` into the slot (`event_ring.cpp`), so nothing survives from a slot's previous occupant.

  **But the id must never be read without dispatching first**, because one publisher does emit
  uninitialised payload bytes: `engine_ui_publish.cpp:145` declares `daw::EventEntry gateEntry;`
  without `{}`, sets `sampleTime`/`blockId`/`type`/`size = 0`, never touches `payload`, and calls
  `ringWrite`. Forty bytes of engine stack are published into a segment other processes read. For that
  entry, bytes 32–40 are garbage that can look like a valid non-zero id.

  So the reader rule is: **read the id only after dispatching on `type`, and only when `size` covers
  offset 40.** That is required regardless of whether `gateEntry` is fixed, because a reader must not
  depend on a publisher's initialisation discipline.

  `gateEntry` is a defect in its own right — an information leak and a source of ring nondeterminism —
  and is reported separately rather than fixed here. It is the only such site: sixteen `EventEntry`
  declarations exist and **none** is `{}`-initialised, but the other fifteen either `memcpy` a full
  40-byte payload or are `ringPop`/`ringPeek` destinations, which are filled by the ring rather than
  published from the stack.
- readers on 38 must not assume a 37 engine's `reserved` bytes are zero.

## 7. Acceptance gates

Unchanged in intent from the original's nine, with the corrections applied. Restated only where they
change:

1. **positive** — a forged refusal carrying my id is adopted.
2. **negative** — a refusal carrying a *different* id is not adopted. Still the test that cannot be
   written today, and still what blocks CTRL-02-B-1.
3. **window** — a refusal predating my send is not adopted even with a matching id.
4. **retry** — a retry mints a new id; the first refusal must not satisfy the second send.
5. **dedupe** — the same refusal peeked twice yields one outcome.
6. **batch** — a bulk of N with one refusal reports *which* command was refused.
7. **legacy** — an all-zero id reports `Unknown`, never `Applied`. Note `ClipOutcome::Applied |
   ClipOutcome::Unknown =>` is a live arm at `main.rs:2990`, `:3001`, `:5419`, `:5431` — **four sites
   to change**, not a comment to update.
8. **silence** — no refusal reports `Unknown` explicitly.
9. **mirror** — the Rust mirror carries the fields at the same offsets, with `==` size assertions.
10. **alignment (new)** — every refusal payload stays `alignof == 4`. Sabotage: change one half to a
    `uint64_t`; sizes and offsets are unchanged and only this gate fires.
11. **uniformity (new)** — the id is at offset 32 in all seven. Sabotage: move one to 24; every other
    gate passes.

## 8. Owner decisions still open

1. **Nonce vs. an exact producer-id claim in SHM** (§3). I recommend the nonce and state its bound.
2. **Whether `commandType` goes on the wire at all.** The review argued it is redundant against a
   unique id and does not fit `UiPatcherGraphErrorPayload` alongside one. This revision drops it; four
   of seven payloads have no `commandType` today and gain nothing from one.
3. **Zero-initialisation of reserved payload bytes** (§6) — a measurement, not a preference, and it
   must be made before implementation.

## 9. What this still does not do

It does not decide whether undo/redo are arbitrated (item 30 / R10). It does not add a scope field. It
does not make the engine wait for anything — the per-segment ack of P12-18-01 is a separate concern on
a different channel, and the two should share **one** minting scheme rather than growing two.
