# P2-CMD-00 — owner decision memo

**State** READ-ONLY. No product, wire or design change. This does not decide anything; it prices the
three choices so somebody who can decide has the numbers.
**Author** claude-worker-1 · 2026-08-13 · against current files at 8c6ee35c.
**Companion to** `P2-CMD-00-revised.md`. Everything below is measured; where it is not, it says so.

---

## Decision 1 — minting: per-process nonce, or an allocated producer id?

### The number that decides it, and it is not the one I used before

The revision priced the nonce against *producer lifetimes* and guessed 100–1000. That was the wrong
denominator. A collision only matters when two producers bearing the same id have refusals **visible
at the same time**, and the ring bounds that directly:

```
daw_engine_main.cpp:277   const uint32_t uiDiffRingCapacity = 1024;
```

The UI-out ring holds **1024 entries**, so at most 1024 refusals — hence at most 1024 distinct
producer ids — can be simultaneously present in the window any reader can see. Older entries are
overwritten whether or not anyone read them. So the denominator is **ring capacity, not session
length**, and it does not grow with uptime.

Birthday on a 32-bit nonce with n = 1024:

> p ≈ n² / (2 × 2³²) = 1,048,576 / 8,589,934,592 ≈ **1.2 × 10⁻⁴** — about 1 in 8,200.

That is an **upper bound** and a loose one: it assumes all 1024 entries come from 1024 *distinct*
producers. In a normal session one sidecar owns most of them, so the realistic n is a handful and the
probability is orders of magnitude smaller. It is worth stating the bound rather than the realistic
figure, because the bound is the one that survives an unusual workload.

**This matters because `daw-cli` is a CLI** — `ui/daw-cli/src/main.rs:2334` is a `main()`, so every
invocation is a fresh process and a fresh nonce. A scripted loop produces thousands of lifetimes an
hour. Under the *lifetimes* denominator that is alarming; under the ring denominator it is irrelevant,
because a finished `daw-cli` has no reader awaiting an outcome and its entries age out. **The ring
capacity is what makes the nonce defensible, and it should be cited wherever the bound is.**

### What the exact alternative costs

An allocated producer id (a CAS-claimed slot array in SHM) makes collision impossible. Its costs,
measured:

- **it is a wire change** — a new SHM region, so `kShmVersion` moves for the *mechanism* as well as
  the payload fields;
- **it needs an allocation authority, and this project already has one to copy**:
  `apps/event_payloads.h:503` reads `SetMarkerColor = 99,  // next free 100`. The enum carries "next
  free" in a comment, and allocation is coordinated between agents by hand. A producer-id registry
  inherits exactly that discipline, plus a problem the enum does not have —
- **stale-claim reclamation**. A `daw-cli` that is SIGKILLed never releases its slot. The registry
  therefore needs liveness (a pid, a heartbeat, a generation), which is a second design with its own
  failure modes. The enum has no equivalent because a command type is never released.

### Recommendation and what would change it

**The nonce**, on the grounds that the exact scheme buys a 1.2 × 10⁻⁴ upper bound down to zero at the
price of a wire change, a hand-maintained registry and a liveness protocol.

I would switch to the exact scheme if **a correlation id ever gates something irreversible**. Today it
selects which refusal a client believes; the failure mode of a collision is one client adopting
another's refusal, and the client's next read corrects it. If an id ever authorises a destructive
action, a probability stops being acceptable at any bound.

---

## Decision 2 — does `commandType` ride the wire?

### Measured

The sender sets it at every send site. `ui/daw-cli/src/main.rs` alone: `:303`, `:385`, `:420`, `:745`,
`:782`, `:870` and more, each `command_type: <a literal UiCommandType> as u16`. It is **a constant the
sender chose seconds earlier**, so with a unique id the sender can recover it from its own send record
without the engine echoing it back.

Against that, it does not fit: `UiPatcherGraphErrorPayload` has exactly 8 reserved bytes, which the id
consumes entirely. Carrying `commandType` in all seven means either dropping it for that payload — a
field present in six of seven is worse than a field in none, because a reader must special-case — or
finding two more bytes there, which do not exist.

### The case for keeping it, stated fairly

It is a **diagnostic**. A human reading an engine log or a raw ring dump sees which verb was refused
without holding the sender's table. Four of seven payloads have no `commandType` today, so that
convenience is already absent from most refusals.

### Recommendation

**Drop it from the wire.** The id is unique; `commandType` is a denormalised copy that can disagree
with it, and a field that fits six of seven payloads forces a special case into every reader.

Cheap mitigation if the diagnostic matters: the *sender* logs `(id, commandType)` when it mints. That
puts the human-readable mapping where the table already is and costs no wire bytes.

---

## Decision 3 — migration and version gates

### The changeset footprint, measured

Six files carry the version constant and must move together:

| file | |
|---|---|
| `apps/shared_memory.h` | `kShmVersion = 37` — the authority |
| `ui/daw-bridge/src/layout.rs` | `K_SHM_VERSION: u16 = 37` — the mirror |
| `apps/engine_startup.cpp` | |
| `apps/host_controller.cpp` | |
| `apps/ipc_protocol.h` | |
| `apps/juce_host_process_main.cpp` | |

Seventeen further files mention the constant without carrying it (docs, checks, `control.rs`,
`daw-sidecar`). They do not need editing, and a search that returns 23 files overstates the work by
almost four times — worth knowing before anyone estimates this.

### The gates

1. **one bump, 37 → 38**, carrying all seven payload changes, both correlation fields, and the
   `SHM_LAYOUT.md` update in a single changeset. Giving a reserved field a meaning is a wire change
   even though no size moves — that is exactly how a wire change lands without a bump.
2. **the mirror moves in the same commit.** `tools/contract_layout_check.sh` exists to catch the two
   sides disagreeing; it should fail on a split changeset, and if it does not, that is a finding.
3. **28 new `static_assert`s** (four per payload: two offsets, `alignof == 4`, `sizeof == 40`), plus
   the same offsets on the Rust side. The `alignof` one is the assertion that catches the mistake §2
   of the revision describes, and nothing in the tree catches it today.
4. **`correlationLo == 0 && correlationHi == 0` → `Unknown`**, never a match, never `Applied`. Safe:
   §6.2 of the revision measured that refusal payloads are `{}`-constructed and fully written.
5. **the reader rule of §6.1 is part of the migration**, not a follow-up: dispatch on `type`, require
   `size ≥ 40`, then read. A 38 reader against a 37 engine must not read offset 32 at all.

### What is not gated, and should be said out loud

Nothing here makes a *37* engine safe to read with a *38* reader beyond the sentinel — a 37 engine's
reserved bytes are simply reserved, and the design assumes a reader checks the header version first.
That check exists; this design does not add one.

---

## Summary for the decider

| | recommendation | what it costs if I am wrong |
|---|---|---|
| **1 minting** | per-process nonce | ≤1.2 × 10⁻⁴ chance a client adopts another's refusal; self-correcting on the next read |
| **2 `commandType`** | drop from the wire | a human reading a raw ring dump must consult the sender's log for the verb |
| **3 migration** | one bump, six files, 28 assertions | a split changeset the layout check should catch |

Decisions 1 and 2 are genuinely yours — neither is decidable by measurement, which is why they are
here. Decision 3 is a plan rather than a choice; it is included because its footprint was overstated
by a factor of four in every earlier discussion, including mine.
