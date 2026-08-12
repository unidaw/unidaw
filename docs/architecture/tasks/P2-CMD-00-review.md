# Adversarial review of P2-CMD-00 (f3f541bc)

**State** REVIEW, read-only. No product or design edit is made by this document.
**Reviewer** claude-worker-1 — **who is also the author of the design under review.**
**Against** current files at 43521663. Every number below is measured, not recalled.

## The disclosure that comes first

I wrote P2-CMD-00. An adversarial review by the author is the weakest form there is: I will re-derive
the same reasoning and find it sound, because it is mine. `protocol_audit` has been sent six review
requests — including this one, at 1786547177766 — and **has never posted a single message to the bus**,
so nothing it was asked to review has been reviewed by it.

The only defence available to me is to refuse to re-read my own argument and instead measure the tree
against every claim. That is what follows, and it found the design **wrong in three of its load-bearing
numbers**. It did not find that by thinking harder; it found it by compiling a program that prints
`offsetof` and `sizeof`.

## Finding 1 — the design covers 2 of 6 refusal channels (severity: high)

The document opens *"Two refusal channels carry different amounts of identity"* and is titled *"one
identity for every refusal"*. There are **six** payloads carrying a refusal in `apps/event_payloads.h`:

| payload | line | has `commandType`? |
|---|---|---|
| `UiSamplerRejectPayload` | 1515 | yes, offset 4 |
| `UiClipRejectPayload` | 1570 | yes, offset 16 |
| `UiChainErrorPayload` | 1893 | **no** |
| `UiRoutingErrorPayload` | 1946 | **no** |
| `UiModErrorPayload` | 2030 | **no** |
| `UiPatcherGraphErrorPayload` | 2111 | **no** |

The design's own file cites `UiChainErrorPayload` in a comment three lines above the struct it does
analyse, so the counter-example was in the text I read while writing it. **Four of six refusals cannot
name the verb they refused**, not one, and a scheme that unifies two of six while promising every
refusal will be the thing a later reader trusts and finds absent.

## Finding 2 — the "16 bytes" budget is measured against the wrong population (severity: high)

The design derives a **16-byte** budget from the two payloads it looked at and concludes that
`uint64_t correlationId` + `uint16_t commandType` — ten bytes — *"fits both payloads' reserved space
with room left."* Measured across all six, by a compiled program rather than by reading:

| payload | size | align | free | first free offset | fits a u64? |
|---|---|---|---|---|---|
| `UiClipRejectPayload` | 40 | 4 | 22 | 18 | yes |
| `UiHarmonyDiffPayload` | 40 | 4 | 16 | 24 | yes |
| `UiSamplerRejectPayload` | 40 | 4 | 24 | 16 | yes |
| `UiChainErrorPayload` | 40 | 4 | 20 | 20 | yes |
| `UiRoutingErrorPayload` | 40 | 4 | 32 | 8 | yes |
| `UiModErrorPayload` | 40 | 4 | 28 | 12 | yes |
| `UiPatcherGraphErrorPayload` | 40 | 4 | **8** | 32 | yes, **exactly** |

The binding constraint is **8 bytes**, not 16. A `uint64_t` fits everywhere — including
`UiPatcherGraphErrorPayload`, whose eight free bytes start at offset 32 and are therefore 8-aligned by
luck. But it consumes **all** of them, so **the proposed pair does not fit the population.** "With room
left" is true of the two payloads I measured and false of the set.

I predicted this one would fail on alignment and it does not; it fails on capacity. Recording that
because the prediction and the measurement disagreed, and the measurement is the finding.

**And the pair is unnecessary.** If the id is unique per attempt, the sender already knows which verb
it minted the id for — `commandType` on the wire is a denormalised copy of something the correlator
holds. Dropping it makes the design fit all six payloads *and* removes a field that can disagree with
the id. The design argued for adding `commandType` to harmony; the measurement says remove it from the
wire entirely.

## Finding 3 — the `<=` → `==` justification is false (severity: medium)

The design requires *"the tightening of `UiClipRejectPayload`'s `sizeof <= 40` to `== 40` (an
inequality cannot detect a field being added, which is the mechanism this design depends on)"*.

`sizeof(UiClipRejectPayload)` is **already exactly 40**. Adding a field makes it 44, and `<= 40` fires.
The inequality *does* detect an added field.

Worse, the mechanism the design actually depends on — **carving a meaning out of `reserved`** — leaves
the size unchanged, so **neither `<=` nor `==` detects it.** The design names the right hazard in its
Versioning section (*"giving a reserved field a meaning is a wire change"*) and then proposes a control
that cannot see it.

The file already contains the tool that can: `static_assert(offsetof(UiClipRejectPayload, diffType) ==
0)`. **Per-field offset assertions** are what detect a reserved field acquiring meaning. A size
assertion never can, in either form. (Incidental: `UiSamplerRejectPayload` is the other `<=`; the other
five are already `==`. The design named one of the two.)

## Finding 4 — the minting rule specifies an input that does not exist (severity: high)

The design closes with what it calls the risk worth naming, and answers it: *"a monotonic per-client
counter with the client's identity in the high bits"*.

There is **no client, session or sender identity** anywhere in `apps/event_payloads.h`,
`apps/shared_memory.h` or `ui/daw-bridge/src/layout.rs` — grepped for `client_id`, `clientId`,
`sessionId`, `session_id`, `senderId`, `sender_id`: zero hits. The scheme's key input has to be
invented, and the ADR presents it as the mitigation that makes the design sound rather than as the
second design problem it is.

Either the SHM gains a sender identity — a real addition with its own allocation question, in the shape
of the `UiCommandType` registry — or minting must be centralised in one process, which contradicts
"the sender mints it".

## What survived

Verified correct against current files:

- **the correlator's triple** — `ui/daw-cli/src/main.rs:1179` reads `u32at(4) == track && u16at(16) ==
  command_type && u32at(8) == sent_base`, which maps exactly to `trackId@4`, `commandType@16`,
  `sentBase@8`. The design's account of the key is right.
- **`Unknown` resolves as success, and it is worse than the design says.** The design calls it
  *"documented as treated as applied"*. It is not documentation: `ClipOutcome::Applied |
  ClipOutcome::Unknown =>` is a live match arm at `main.rs:2990`, `:3001`, `:5419` and `:5431`. Four
  sites, not a comment. This is the strongest part of the design's motivation and it understated it.
- **both analysed payloads are full at 40 bytes**, so neither can grow — correct, and now known to be
  true of all six.
- **the ring is peeked, not drained**, so dedupe without an id is a time window — unchallenged.

## What I could not review

- Whether `correlationId == 0` as the legacy gate is safe **depends on the ring's initialisation**: a
  zeroed payload is indistinguishable from a legacy sender. I did not verify how `reserved` bytes are
  initialised on the engine side for the four payloads the design never examined.
- The nine acceptance gates are untested claims about a design that changed under this review; they
  should be re-derived after the scope correction rather than carried.

## A defect in this review's own process

My citation script printed `MISMATCH ui/daw-cli/src/main.rs:5423` and I committed anyway, because it
printed rather than exiting non-zero. The count of four was right and one line number was wrong — the
fourth arm is at `:5431`. Corrected above.

That is the same shape as the finding in §3: a control that reports rather than gates is a control the
author can walk past, and I walked past this one within a minute of writing that sentence. A verifier
whose failure path is a `print` is a log line, not a gate.

## Recommendation

**Do not implement as written.** Three of four load-bearing numbers are wrong, and the corrections
change the shape:

1. Scope to **six** payloads, not two.
2. **`uint64_t correlationId` only** — 8 bytes, fits all six, and `commandType` on the wire is
   redundant against a unique id.
3. Replace the size-assertion tightening with **per-field `offsetof` assertions** on every payload
   that gains the field.
4. Answer sender identity **before** the minting rule, as its own decision.

None of this touches the design's core judgement, which I still believe: one opaque sender-minted id,
echoed and never interpreted. The error was not the idea. It was measuring two members of a
six-member population and writing the conclusion in the voice of the whole.
