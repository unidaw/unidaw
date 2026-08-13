# CMD00 step 2 — how the id reaches the engine

Status: DESIGN, for independent review. No code. Written because implementing step 2 found that
none of the four `P2-CMD-00-*` documents answers this question.

## The gap

`P2-CMD-00-revised.md` §4 is "What the engine does with it: nothing — echo verbatim", which presumes
the engine RECEIVES the id. §3 specifies minting, §1 measures the seven refusal payloads' free space
to the byte. **No CMD00 document says where the id rides on the way IN.** Checked all four:
`-command-outcome.md`, `-review.md`, `-revised.md`, `-owner-decisions.md`. Gates 1 (a refusal
carrying my id is adopted) and 4 (a retry mints a new id) are unreachable without an answer.

## The constraint, measured

```
EventEntry payload capacity   44 bytes   (sizeof 64, payload at offset 20)
UiCommandPayload              40 bytes   align 4, eleven fields, NO reserved run
free                           4 bytes   half of an 8-byte id
```

So the refusal side's answer — "take it from the reserved space" — is not available here.

## The proposal: correlate by the reservation index, which both sides already know

`RingHeader` (`apps/shared_memory.h`) carries `ShmAtomicU32 writeIndex`. The ring is multi-producer
since M2.18: a producer CAS-reserves a slot by incrementing that index, fills the entry, then stores
`ready = 1`. So **every command already has a unique 32-bit identity, minted by the reservation
itself**, and:

- the SENDER knows it — it is the value its own CAS returned;
- the ENGINE knows it — it is the index it read the entry from;
- it is monotonic in shared memory, so it survives producer restarts, which is the property §3
  invented the nonce to obtain;
- it needs **no command-side field**, so it is not a wire change and does not enter the gated
  layout path at all.

The engine echoes it into the refusal's existing 8 bytes at offset 32 (`correlationLo` = the index,
`correlationHi` = 0 for now, or a ring epoch — see open question 2).

### Why this is better than the ruled scheme, and why that matters

Owner ruling 1 (2026-08-13) chose a per-process nonce, whose stated cost is birthday-on-2³²:
~1×10⁻⁶ collision at 100 producer lifetimes, ~1×10⁻⁴ at 1000. **That ruling was made before anyone
established that the id has nowhere to ride.** The reservation index is exact rather than
probabilistic, and needs no carrier.

Its ambiguity window is different in kind: `writeIndex` wraps at 2³², so two commands share an
identity only if 2³² commands separate them. At 1,000 commands/second that is ~50 days of
continuous traffic between a send and the read of its refusal. A refusal is correlated within a
window of seconds.

**This supersedes ruling 1 if accepted, which is an owner call and not mine.** It is offered because
the ruling answered "how is an id minted" without knowing that "where does it travel" had no answer.

### What it costs

`ringWrite()` returns `bool` and discards the reserved index. The sender needs it, so the reservation
API must return it — in `apps/event_ring.h` and its Rust counterpart. That is a source change to a
hot path's signature, NOT a wire change: no struct grows, no offset moves, `kShmVersion` does not
move again.

## The alternatives, so the choice is deliberate

- **A. Send only the 32-bit counter on the command.** Fits the 4 free bytes. But the engine does not
  know the producer's nonce, so the high half must be registered somewhere — which is the allocation
  authority §3 rejected as the reason the original scheme died.
- **B. Narrow a field in `UiCommandPayload` to free 4 more bytes.** `notePitch` is a `uint32_t`
  carrying a MIDI pitch. A wire change to the hottest command struct; every consumer moves; another
  version bump; and it re-enters the gated path.
- **D. Grow `EventEntry` past 64 bytes.** It is `alignas(64)` and asserted to be one cache line
  (`apps/shared_memory.h:462`). Rejected.

## Open questions for the reviewer

1. Does every refusal path actually know the reservation index of the command that caused it? The
   engine reads an entry, dispatches on `commandType`, and may refuse deep inside a handler. If the
   index is not threaded through to the emit site, this proposal costs a plumbing change of unknown
   size — that is the risk that would sink it, and I have not measured it.
2. Should `correlationHi` carry a ring epoch (incremented when the ring is created) so an id is
   unambiguous across engine restarts, or is "the refusal history dies with the ring" sufficient?
3. `ringWrite` is used by more than the UI command ring. Does returning the index change any other
   caller's contract?
