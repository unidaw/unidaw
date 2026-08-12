# P2-CMD-00 — one identity for every refusal

**State** DESIGN, read-only. No wire, layout or product change is made by this document.
**Author** claude-worker-1 · 2026-08-12
**Supersedes nothing.** Blocks CTRL-02-B-1, and should be decided together with P12-27-01.

## Why this exists

Two refusal channels carry different amounts of identity, and a client cannot correlate either one
reliably. Measured in current files:

| | `UiClipRejectPayload` | `UiHarmonyDiffPayload` |
|---|---|---|
| identity fields | `trackId`, `sentBase`, `commandType` | none |
| set on a refusal | all of the above | `diffType`, `harmonyVersion` only |
| size pin | `sizeof <= 40` (:1580) | `sizeof == 40` (:2172) |
| free space | `reserved` u16 + `reserved2[5]` = 22 B | `reserved0..3` = 16 B |

Both are already 40 bytes — the `EventEntry` payload capacity — so **neither can grow**. Anything
added must come out of the reserved space, and any unified field must fit in **16 bytes** to be
addable to both.

The clip correlator (`ui/daw-cli/src/main.rs:1179`) keys on `(trackId, commandType, sentBase)`. Its
own comment records that this triple plus a ring window once re-sent an already-applied edit. For
harmony none of the three exists, so correlation is not merely weak — it is impossible.

And `ClipOutcome::Unknown` (`main.rs:1152-1158`) is documented as *"treated as applied"*. So today's
default resolves silence as success on both channels.

## The decision

**One sender-minted opaque correlation id, echoed verbatim in every refusal.**

```
uint64_t correlationId    the sender mints it; the engine NEVER interprets it, only echoes it
uint16_t commandType      already present on ClipReject; added to harmony so a refusal names its verb
```

Ten bytes. Fits both payloads' reserved space with room left.

**The engine must not interpret the id.** Any interpretation makes it a second version counter and
reintroduces the arbitration this is meant to sit beside. Echo only.

### What this buys, per problem

- **correlation** — the id is unique to one attempt, so a refusal answers exactly one command. Scope
  and base become diagnostics rather than keys; they stay for the human reading a log.
- **dedupe** — the same id seen twice is the same refusal. The ring is *peeked*, not drained
  (the real UI is its consumer), so old refusals stay visible indefinitely; without an id, dedupe is
  a time window and nothing more.
- **retry** — a retry mints a **new** id. The old refusal therefore cannot satisfy the retry, which
  is the bug the clip correlator's comment records.
- **batch** — each command in a `send_bulk` mints its own id, so a batch yields **per-command**
  outcomes. This dissolves CTRL-02-B's family 4 without a separate outcome shape.
- **`Unknown`** — becomes precise: *no refusal bearing my id arrived in my window*. It must stay
  distinct from `Applied`. Reporting it as applied is the current behaviour and is what lets a lost
  edit read as a success.

### Versioning

Giving a reserved field a meaning **is a wire change** — the contract is what the fields mean, not
how many bytes they occupy. "It fits in the padding" is how a wire change lands without a bump.

- **one coordinated `kShmVersion` bump** carrying: both payload changes, the harmony `commandType`,
  and the tightening of `UiClipRejectPayload`'s `sizeof <= 40` to `== 40` (an inequality cannot
  detect a field being added, which is the mechanism this design depends on)
- `SHM_LAYOUT.md` and the C++↔Rust mirror (`ui/daw-bridge/src/layout.rs:2311`) in the **same
  changeset**, per the implementation constraints
- **not** `kControlVersion`: the diffs ride the UI ring, not the host ring

### Migration

`correlationId == 0` means **no id** — a sender that predates this. A reader must treat 0 as
**uncorrelatable and report `Unknown`**, never as a match. That is the migration gate: a legacy
sender degrades to today's behaviour and a new sender gets real outcomes, with no window in which an
unidentified refusal is attributed to somebody.

## Acceptance gates

Each is stated so it can be seen to fail, and the negative half is the one that matters.

1. **positive** — a forged refusal carrying my id is adopted.
2. **negative** — a forged refusal carrying a *different* id is **not** adopted. This is the test
   that cannot be written today, on either channel; its absence is what blocks CTRL-02-B-1.
3. **window** — a refusal predating my send is not adopted **even with a matching id**. Guards
   against an id reused across attempts.
4. **retry** — after a retry mints a new id, the *first* refusal must not satisfy the *second* send.
5. **dedupe** — the same refusal peeked twice yields one outcome, not two.
6. **batch** — a bulk of N with one refusal in the middle reports **which** command was refused. A
   result that can only say "something failed" fails this gate.
7. **legacy** — a refusal with `correlationId == 0` reports `Unknown`, never `Applied` and never a
   false match.
8. **silence** — no refusal at all reports `Unknown` explicitly. `main.rs:1421` already prints
   `"applied": "unknown"` for one verb and is the model.
9. **mirror** — the Rust mirror carries the same fields at the same offsets, and the size assertion
   is `==` on both payloads.

## What this does not do

- It does not decide whether undo/redo are arbitrated. That is item 30 / R10, and a correlation id
  on a command the engine does not arbitrate would report refusals that cannot happen.
- It does not add a *scope* field. The id is unique; scope would be a second key that can disagree
  with the first.
- It does not make the engine wait for anything. This is identity, not acknowledgement — the
  per-segment ack of P12-18-01 is a separate concern on a different channel, and the two should share
  **one** minting scheme rather than growing two.

## The risk I would flag to a reviewer

Every earlier attempt in this area failed by being *almost* able to correlate — three keys that
worked until they didn't, a version that meant two things, a reserved field that acquired a meaning
quietly. The failure mode of this design is different and worth naming: an opaque id is only as good
as the sender's minting discipline. If two senders can mint the same value, the id is a scope key
with extra steps. So the minting rule belongs in this ADR before implementation: **a monotonic
per-client counter with the client's identity in the high bits**, and the reason a bare random or a
bare counter is insufficient stated where the field is defined, not in a commit message.
