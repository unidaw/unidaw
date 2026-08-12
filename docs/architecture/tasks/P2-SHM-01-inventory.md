# P2-SHM-01 — `EventEntry` ABI parity: THREE types share one name

**Read-only. No edits.** Measured from current files; the C++ offsets below were produced by
compiling, not derived by hand.

## DISAMBIGUATE BY PATH FIRST — this is where this question has been answered wrongly before

`git grep -n "struct EventEntry"` returns **three definitions**, and they are not all the same type:

| # | path | members | has `ready`? |
|---|---|---|---|
| 1 | `apps/shared_memory.h:433` (C++) | 7 | **yes**, at offset 60 |
| 2 | `ui/daw-bridge/src/layout.rs:702` (Rust) | 7 | **yes** |
| 3 | `patcher_rust/src/lib.rs:98` (Rust) | 6 | **NO** |

**Any statement about "the Rust EventEntry" is ambiguous and must name its path.** A previous
attempt at this question read #3 and concluded a hazard in #1/#2 did not exist.

## THE GOVERNED PAIR (#1 ↔ #2) AGREES, AND IS ALREADY PINNED

C++ measured (`c++ -std=c++20 -I. ` over `apps/shared_memory.h`):

    sizeof=64  alignof=64
    sampleTime=0  blockId=8  type=12  size=14  flags=16  payload=20  ready=60

Rust bridge already asserts both facts that matter:

    ui/daw-bridge/src/layout.rs:2487   const_assert_eq!(size_of::<EventEntry>(), 64);
    ui/daw-bridge/src/layout.rs:2495   assert_eq!(offset_of!(EventEntry, ready), 60);

Member NAMES differ by convention (`type` / `event_type`, `sampleTime` / `sample_time`) and that is
not an ABI concern; the offsets are. **Nothing is required for this pair.**

## THE THIRD TYPE IS THE OPEN QUESTION

`patcher_rust/src/lib.rs:98` has six members ending at `payload: [u8; 40]`, so with
`#[repr(C, align(64))]` its bytes 60..64 are **implicit padding** exactly where #1 and #2 keep
`ready`. And it is written WHOLE-OBJECT:

    patcher_rust/src/lib.rs:150-158
    unsafe fn push_event(ctx: &mut PatcherContext, entry: EventEntry, ...) {
        let slot = ctx.event_buffer.add(count as usize);
        *slot = entry;

A whole-object store through a type whose tail is padding writes those four bytes with whatever the
padding holds. **If that buffer is read by a consumer that requires `ready == 1`, every entry the
patcher writes is either ignored or racy.**

### WHAT DECIDES IT, AND WHY I HAVE NOT DECIDED IT

`ctx.event_buffer` is a `*mut EventEntry` handed in through `PatcherContext` alongside
`event_count` and `event_capacity`. **Whether that points at the multi-producer command ring — where
`ready` is the publication flag — or at a per-block scratch buffer whose consumer never reads
`ready`, is the whole question**, and I have not traced it.

I am not asserting the hazard. Asserting it from the struct shape alone is precisely the error this
document opens by warning about: the shape is suggestive and the BUFFER IDENTITY is what matters.

## BOUNDED IMPLEMENTATION TICKET — P2-SHM-02

**Step 1, read-only and blocking:** trace `PatcherContext::event_buffer` to its producer — who
allocates it, who consumes it, and whether that consumer gates on `ready`. One answer, and it
decides everything below.

**If it IS the ready-gated ring**, the fix is in the patcher crate: give #3 an explicit `ready` field
so the whole-object store writes a defined value, and set it last. Negative controls: a patcher-written
entry must be visible to the C++ consumer; and reverting the field must make it invisible — the
second is the one that proves the control, since the first passes today if the buffer is not gated.

**If it is NOT**, the correct outcome is a comment on #3 saying which buffer it serves and that it is
deliberately not the ring's type — because the next reader will ask this same question, and today
nothing in either file answers it.

**Required in both branches:** a parity assertion for #3 like the bridge's, pinning its size and the
absence of `ready`, so the two Rust types cannot silently converge or diverge. There is none today —
`git grep` for `offset_of` in `patcher_rust` returns nothing.

Related: AE-P1.2 item 35 (frozen packet) records the same third-type finding as PRODUCT/BLOCKING.
This was measured independently and agrees on the shape; it does not agree that the hazard is
established, because item 35 does not name the buffer either.
