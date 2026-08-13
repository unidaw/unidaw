# AE-RING-01 — a retired slot can be resurrected, replaying a stale command

Status: DEFECT, verified by reading the code. Not fixed: the clean fix changes the meaning of a
field shared by two independent ring implementations, which is exactly the doctrine question open as
owner decision 5.

Found by the independent reviewer of the CMD00 carrier design, as a side finding while refuting a
different proposal. It is live today and unrelated to CMD00.

## The mechanism, verified

M2.18 made the rings multi-producer: a producer CAS-reserves a slot on `writeIndex`, fills the
entry, and only then stores `ready = 1`. The consumer refuses to read a slot until it is ready
(`apps/event_ring.cpp:110-115`).

A producer that dies between reserving and publishing would stall the ring forever, so the consumer
watches for it and retires the slot after a grace period:

```
apps/engine_ui_thread.cpp:50    kStalledSlotGraceMs = 2000
apps/engine_ui_thread.cpp:54-78 watch, log ring.abandoned_slot, then ringSkipStalledSlot()
apps/event_ring.cpp:133-140     storeReady(entries[read].ready, 0); readIndex = (read + 1) & mask
```

**The grace period assumes the producer is dead. It only establishes that the producer is slow.**

A producer that is merely slow — a page fault, scheduler pressure, a debugger, SIGSTOP — publishes
after the skip. It writes `ready = 1` into a slot the consumer has already retired and moved past.
Nothing tells it the slot is gone; `ringWrite` has no post-reservation validation.

One lap later — **1023 commands, which at 1,000 commands/second is about one second**, since
`writeIndex` is masked at capacity 1024 (`apps/event_ring.cpp:79`, `apps/engine_startup.cpp:232`) —
`readIndex` returns to that slot. `ringPeek` (`apps/event_ring.cpp:110`) sees `ready != 0` and
accepts the entry. **The stale command is dispatched.**

The consequence is not a dropped command, which the skip already accepts as its price. It is a
command from a second ago being executed again, out of order, after the sender has given up on it.

## Why this is not merely theoretical here

The ring is 1024 entries and the wrap is ~1s under load, so the resurrection window is short and
frequently reached rather than astronomically rare. And the skip path is not hypothetical: it is
instrumented (`ring.abandoned_slot`), which means it was built because it fires.

## The fix, and why it is not applied here

A lap tag. Instead of `ready` being `{0, 1}`, a producer publishing at write index `W` stores a
non-zero value derived from `W`'s lap, and the consumer at `readIndex R` accepts only when the value
matches `R`'s lap. A late publish then carries the previous lap's tag and is rejected rather than
resurrected. `ready` is already a `uint32_t` (`apps/shared_memory.h`, in what was tail padding), so
this needs **no layout change** — no field grows, no offset moves.

But it changes what the field MEANS, and `ready` is read and written by two independent
implementations: `apps/event_ring.cpp` and the Rust reimplementation at
`ui/daw-bridge/src/control.rs:1904+`. Both must change together, and by the doctrine CMD00 states
three times — "giving a reserved field a meaning IS a wire change" — that is a versioned change.
The tree also carries nine counter-precedents saying such repurposing needs no bump. **That
contradiction is owner decision 5, and this ticket should not pre-empt it by picking a side.**

## What to do

1. Settle decision 5.
2. Then implement the lap tag in both implementations in one changeset, with a control that
   reproduces the resurrection: reserve a slot, retire it via the grace path, publish late, run a
   full lap, and assert the entry is NOT dispatched. That control fails today.
3. Consider separately whether `ringWrite` should validate its reservation before publishing, which
   would let a slow producer discover it was retired rather than only be prevented from doing harm.
