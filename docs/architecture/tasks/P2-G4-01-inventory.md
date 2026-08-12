# P2-G4-01 — adjacent multi-plugin ownership: what exists, and what the fixture must build

**Read-only inventory. No product or SHM edits.** Measured from the current checkout; every claim
names the file and line it came from, and no statement here is taken from git history.

## THE INVARIANT, AND WHERE IT ACTUALLY LIVES

G4's subject is a consumer reading bytes ANOTHER agent wrote. For adjacent plugins that other agent
is the host process, and the crossing happens **inside one host, between two plugins in one
segment** — not between engine and host.

**The segment is built engine-side.** `engine_produce_block.cpp:660-680` walks
`trackState.chainDevices` and accumulates `segmentStart` / `segmentLength`; consecutive hosted
plugins land in ONE segment and are dispatched as one request.

**The rebind is host-side, and it is one line.** `juce_host_process_main.cpp:1061`:

    inputPtrs = outputPtrs;
    numInputs = numOutputs;

at the bottom of the per-plugin loop (`:866` onward, `for (index = segmentStart; index <
segmentEnd; ++index)`). So plugin N+1's INPUT is plugin N's OUTPUT BUFFER — the same memory,
re-pointed, with no copy and no fence between them.

**That is the ordering question.** The engine's write -> release -> acquire -> read discipline is
between ENGINE and HOST: the host publishes with
`state.mailbox->completedBlockId.store(request.blockId, std::memory_order_release)`
(`juce_host_process_main.cpp:1085`) and the engine reads it through the TrackInfo snapshot
(`engine_consumer.cpp:655`, consumed by `completedMinimum` at `engine_rt_helpers.cpp:496-510`).
**Between two adjacent plugins there is no such pair** — they are sequential calls on one thread,
so the ordering is program order and the hazard is not a race but ALIASING: whether plugin N+1 may
assume its input is immutable while it writes its output into the same buffer.

## INPUT IMMUTABILITY — the actual open question

`inputPtrs = outputPtrs` means the second plugin reads and writes the same channel buffers. Whether
that is safe depends on each plugin's own in-place behaviour, which the host cannot see and does not
currently assert. Note `juce_host_process_main.cpp:884` already alternates buffers by parity —
`if (((index - segmentStart) % 2) == 0)` — which suggests double-buffering IS happening for some
part of the path; **the relationship between that parity swap and the `:1061` rebind is the first
thing an implementer must read**, and I have not traced it. It decides whether the aliasing above
is real or already handled.

## THE FIXTURE GAP, MEASURED

**No fixture in the tree builds an adjacent hosted-plugin pair.** Commands run:

    grep -c 'add-device' tools/multiout_check.sh tools/sidechain_check.sh \
                         tools/multi_producer_ring_check.sh      -> 0, 0, 0
    git grep -ln "LoadPluginOnTrack" -- tools                    -> op_registry_check.sh,
                                                                    undo_ratchet_check.sh

and both of those are string mentions in a classification table, not invocations. Every fixture
that touches a chain uses `add-device --kind sampler`, which is the built-in sampler, not a hosted
VST — so nothing exercises two HOSTED plugins in one segment.

**The fixture must therefore be built, and it needs:** a track with two hosted plugins adjacent in
the chain (so `segmentLength >= 2`), a signal distinguishable per stage (so "plugin 2 read plugin
1's output" is provable rather than assumed), and a deterministic oracle over the result. The
oracle is the hard half and is the reason AE-P1.2 item 26 is still open.

## DEPENDENCIES ON HOST01 / HOST02

**None for the fixture; one for the oracle.** The crossing is intra-host and intra-thread, so it
does not need host identity or generation binding to OBSERVE. But if the oracle is expressed as
"the engine saw plugin 2's output for block N", it reads `completedBlockId` through the snapshot —
and after a host relaunch that snapshot belongs to a NEW SharedMemoryView (`host_controller.cpp:286`,
`:294`). **An oracle that survives a mid-test host death needs HOST02's generation binding; one that
asserts a clean run does not.** Splitting on that is what keeps this disjoint.

## A DISJOINT IMPLEMENTATION SPLIT

1. **Read the parity swap against the rebind** (`juce_host_process_main.cpp:884` vs `:1061`) and
   state whether adjacent plugins alias. Read-only. **Everything below is conditional on this**, and
   it is the step I did not do.
2. **The fixture**: two hosted plugins on one track, per-stage-distinguishable signal. No engine
   changes. Independent of HOST01/HOST02.
3. **The clean-run oracle**: assert stage 2 saw stage 1's output for a given block. Needs (2). Does
   NOT need generation binding.
4. **The survives-a-relaunch oracle**: needs HOST02. Explicitly LAST, and separable.

## WHAT I DID NOT DO

I did not trace the parity swap, so I cannot say whether the aliasing at `:1061` is a live hazard or
already handled — and that single fact determines whether steps 2-4 are testing a defect or
documenting correct behaviour. It is step 1 for exactly that reason.

Related: AE-P1.2 item 26 (frozen packet) records the same fixture gap; this was measured
independently from the current checkout and agrees with it.

## STEP 1 ANSWERED: THE ALIASING IS NOT A LIVE HAZARD — the ping-pong prevents it

Traced `juce_host_process_main.cpp:866-900` (loop head and buffer selection), `:1061` (the rebind)
and `:219-227` (the allocation). **The concern I raised above is wrong, and here is why.**

**The output buffer is chosen by parity, and the input is always the OPPOSITE parity.**

    outputPtrs = isLast ? state.outputPtrs.data()
                        : ((index - segmentStart) % 2 == 0 ? chainPtrsA.data()
                                                           : chainPtrsB.data());   // :869-872
    ...
    inputPtrs = outputPtrs;                                                        // :1061

So for a segment of N:

| index | writes to | reads from |
|---|---|---|
| 0 | `chainPtrsA` | the segment input (`state.inputPtrs`) |
| 1 | `chainPtrsB` | `chainPtrsA` |
| 2 | `chainPtrsA` | `chainPtrsB` |
| last | `state.outputPtrs` | whichever chain buffer the previous wrote |

**Input and output are never the same memory for any adjacent pair.** `chainBufferA` and
`chainBufferB` are two distinct `std::vector<float>` (`:87-88`), and `chainPtrsA/B` index into their
own buffer (`:223-227`). `inputPtrs = outputPtrs` hands the next iteration the buffer just written,
and the next iteration selects the OTHER one to write into. That is a ping-pong, and it is exactly
the structure that makes in-place aliasing impossible here.

**The pre-clear does not break it either.** `:883-888` zeroes the buffer this iteration is about to
WRITE, only when `!isLast`. At index 2 that clears `chainBufferA` while the input is `chainPtrsB` —
so the clear never touches the input it is about to read.

### CONSEQUENCE FOR THE PLAN ABOVE

**Steps 2-4 would document correct behaviour, not catch a defect.** That is a materially different
task and should be scoped as one — a regression guard on a property that currently holds, worth
having and worth pricing honestly, rather than a hunt for a bug that is not there.

### THE RESIDUAL I AM NOT CLOSING

**The single-plugin segment.** When `segmentLength == 1` the only plugin is `isLast`, so it reads
`state.inputPtrs` and writes `state.outputPtrs` — and whether THOSE alias is an engine-side question
about how the planes are bound, not a host-side one. It is outside the adjacency question and I have
not checked it. It is also the case a two-plugin fixture would never exercise.

### BOUNDED NEXT TICKET

**P2-G4-02 — a regression guard on the chain ping-pong.** Assert that for a segment of two or more,
each plugin's output buffer differs from its input buffer, and that the pre-clear never zeroes the
buffer about to be read. Cheap, needs no host fixture (the selection is arithmetic over `index`,
`segmentStart` and `isLast`), and it pins the property that makes adjacency safe TODAY — which is
what would silently break if someone "simplified" the parity away. **That is a better use of the
effort than the multi-plugin fixture**, and it does not need HOST01 or HOST02 at all.
