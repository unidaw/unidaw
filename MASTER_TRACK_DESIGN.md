# Master track (patcher-is-a-device, item 4) — design + open decisions

Status: **designed, not implemented.** Items 1–3 (drop track-level patcher → head
device; resolve the patcher_node_id sentinel so device patchers run + publish
walkably; per-device `generates` + MIDI/audio flow on the chain snapshot) are done
and verified — they close the phantom-notes loop. Item 4 is follow-on feature work
that turns on a genuine architecture/product decision, captured here.

## What was asked

> A MASTER TRACK. Addressable, with a device chain like any other track, whose
> output is the master bus. Today there is a master OUTPUT in the audio path
> (`m_masterChannels`, the virtualMaster path) but nothing a UI can select or put a
> device on. This is what makes the rule ["a patcher is a device"] hold with no
> exceptions: the legitimate "global patcher" case — graph logic that is not
> per-part — becomes a patcher device on the master's chain, opened exactly like any
> other device. No new pane, no graph without a visible home.

## Where the master lives today

`EngineAudioCallback::process` (apps/daw_engine_main.cpp) sums each track's rendered
audio into `master[ch]` — either straight into the device output buffer or into the
virtual `m_masterBuffer` when the mix is wider than the device (surround). There is
**no track, no chain, no per-block processing hook** on that sum. The per-track
plugin chains run OUT OF PROCESS and are PRE-RENDERED ahead of the callback
(numBlocks pipeline) precisely so the RT callback never blocks on a host round-trip.

## The fork: what does a device on the master DO?

Two very different capabilities hide under "a device chain on the master":

**(A) A global PATCHER (event / modulation) device.** The frontend's stated primary
driver — "a home for graph logic that is not per-part." This does NOT touch the
master audio sum. It runs in the existing per-track patcher execution model. LOW
architectural risk. The only real question is semantic: what does a master patcher's
output DRIVE? Options: global modulation (LFO/macro → targets on other tracks via
mod links — already representable), or a global event stream that needs a sink. An
event generator on master with no instrument is silent, so "generator on master"
only makes sense paired with an instrument on master (which needs (B)) or as a
modulation source.

**(B) A VST AUDIO EFFECT on the master sum** (bus compressor, limiter, global EQ).
This is what "output is the master bus" most literally means, and it is the hard
part: the sum is not known until callback time, so an out-of-process host on the
master cannot pre-render like the per-track pipeline. Three ways to do it, each a
real tradeoff:
  - **B1 In-callback SHM round-trip to a master host.** Push the sum to a master
    host, block for the processed result inside the callback. Simplest mental model,
    but adds a synchronous IPC hop to every audio callback — the exact thing the
    low-latency work removed. Needs a hard deadline + dropout policy.
  - **B2 One-block-latency master.** Feed the master host block N, output its result
    for block N-1. Bounded, RT-safe (no in-callback blocking), costs one block of
    latency on the master only. PDC already exists to compensate. **Recommended if
    (B) is wanted.**
  - **B3 In-process master effects only.** A separate, in-process effect class for
    the master (no out-of-process host). Fast, but breaks the "every plugin is a
    device hosted the same way" model and can't host arbitrary VST3s. Not
    recommended.

## Also a product decision: is the master a real track?

- **Track-list presence.** Is master a normal entry in `uiTrackId`/`uiTrackName`
  (a reserved id, e.g. the high end of the slot space or a dedicated flag), or a
  distinct addressable entity the UI special-cases? kUiMaxTracks is 8 and already
  budget-constrained (see child tracks); a reserved master id eats one slot.
- **Arrangement.** The master has no clips/placements. It should be a mixer/chain
  entity, not an arrangement lane — so it needs the flag vocabulary to say "chain +
  mixer, no rail."
- **Addressing.** Commands (AddDevice, SetChain, RequestDevicePatcher, mixer) must
  target the master id. Reusing the existing per-track command path with a reserved
  id is the cheap route.

## Recommendation

Split item 4 along the fork:

1. **4a — master as an addressable chain host for PATCHER devices (capability A).**
   Reserve a master track id, publish it (track-list flag: chain+mixer, no rail),
   route the existing per-track command path to it, and run its patcher in the
   existing model. Delivers the frontend's stated primary purpose — a visible home
   for a global patcher — with LOW risk and no RT-audio change. Resolve the "what
   does a master patcher drive" semantic first (recommend: modulation source +
   optional instrument-on-master once 4b lands).

2. **4b — VST audio effects on the master sum (capability B), via B2**
   (one-block-latency master host, PDC-compensated). Separate, larger increment;
   land after 4a and after an explicit latency sign-off.

## Decision needed (frontend / Jaakko)

- Is the immediate need **(A)** a home for a global patcher, **(B)** audio FX on the
  master sum, or both?
- If (B): accept **B2's** one-block master latency (recommended), or require B1's
  zero added latency (and its dropout risk)?
- Master as a **reserved track id** in the existing arrays (costs one of 8 slots),
  or a **separate addressable entity**? If the former, do we lift kUiMaxTracks first?

Implementation is a clear, mechanical follow-up once these are answered; nothing
here is blocked on engine capability, only on the intended behaviour.
