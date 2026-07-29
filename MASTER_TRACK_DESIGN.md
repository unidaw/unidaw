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

## DECISION (Jaakko, 2026-07-29): build BOTH A and B.

**A (a home for a global patcher) is DONE end-to-end.** The master is a complete,
usable strip:
- 4a-1 (953f251): published addressable entity — `kMasterTrackId` (0xFFFF0000),
  `kUiTrackFlagMaster`, compacted after the regular tracks; the UI keys on the id.
- 4a-2 (a45dff9): its device chain takes edits — `do add-device --track master ...`.
- 4a-3 (d3cfc63): the master fader — its gain/mute attenuates the summed output
  (mute -> flux 0 / pk 0.00).
- 4a-4 (938fb41): persistence — the master's chain survives save/reload (an
  `is_master` track entry lifted out on load).
All verified by `tools/master_track_check.sh` (addressable + takes edits + persists).
A patcher device on the master runs in the existing per-track patcher model.

**B (audio FX on the sum) is the only remaining piece** — it needs the latency call
below before the RT callback change.

## 4b implementation plan — audio FX on the master SUM (B2, one-block latency)

The reusable pieces (all already in the engine):
- **Audio-in hosts.** `TrackRuntime::inputAudioChannels` is an audio buffer the
  producer feeds a host as its input (built for sidechain / patcher-audio,
  daw_engine_main.cpp:1688, :9944). The master host is an effects chain whose input
  is the master sum — the same shape.
- **Per-track audio rings.** A host writes block N to `slot N % numBlocks`; the
  callback reads it (the "host writes block N to slot N%numBlocks" mix loop).
- **`rebuildHostForChain`** spawns/reconciles a host for a runtime's chain.

The constraint: the master's input (the sum) is known only at CALLBACK time, but the
producer feeds hosts AHEAD (the numBlocks pipeline). So the master host runs ONE
BLOCK BEHIND:

  1. **Set up the master host.** Give `masterTrack` a real host config
     (socket/shm/inputAudio like a normal track — 4a created it bare) and call
     `rebuildHostForChain(masterTrack)` when its chain has VstEffect devices. Stop
     skipping the rebuild for the master once this is wired.
  2. **Callback → producer hand-off.** After the callback finishes summing into
     `master[]` (daw_engine_main.cpp ~856), copy that sum into a lock-free
     single-slot double-buffer (`m_masterSumForFx`). No allocation, plain stores.
  3. **Producer feeds the master host.** The producer loop copies the latest handed-
     off sum into `masterTrack->inputAudioChannels` and drives the master host's
     render exactly as it drives a track's — the host processes sum[N-1] → master
     out[N-1] into the master audio ring.
  4. **Callback consumes the processed master.** When the master host is ready, the
     callback, instead of sending `master[]` straight to the device, reads the master
     host's output ring (block N-1) and outputs THAT; it still writes the fresh sum[N]
     to the hand-off. Bypass path (no master FX / host not ready) = today's behaviour
     exactly, so this is safe to land dark and flip on when the chain is non-empty.
  5. **Latency.** The master output is one block late — uniform added output latency,
     nothing to PDC against (PDC aligns tracks against each other; here everything is
     equally delayed). Report it in the latency line.

Risk is concentrated in step 4 (the callback's output source changes). Gate it hard
on "master has an enabled VstEffect AND its host is ready", so a project with no
master FX takes the identical path it does today. Verify with the capture loop:
master with a known effect (e.g. a gain/limiter) must change the sum measurably vs
bypass, and underrun telemetry must stay at zero.

This is the next focused increment — deliberately not rushed into the RT callback in
the same pass as 4a, since a wrong handshake there reintroduces the dropouts the
low-latency work removed.
