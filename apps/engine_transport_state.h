#pragma once

// THE TRANSPORT — where the playhead is, whether it is moving, and what it loops over.
//
// The second engine object of #26, and the one that pays the most in coupling. These six atomics
// appeared as TWENTY-FIVE separate members across ten *Deps structs — ChainCommandDeps and
// DeviceCommandDeps and XrunReporterDeps each want `playing`, ClipEditDeps wants the loop bounds,
// ProducerBlockDeps wants five of the six, TransportCommandDeps wants all six — because every
// module that asks "are we playing" or "where does the loop end" had to be handed each piece
// individually. Ten HarmonyTimeline-shaped references replace them: 25 members become 10.
//
// PURE STATE, NO BEHAVIOUR, AND THAT IS DELIBERATE. There are transport COMMANDS already, in
// apps/engine_transport_commands.h — play, stop, position, tempo, loop range, panic, quit. They
// are not methods here. A command validates a payload, journals, emits and refuses; this holds
// six numbers the audio callback and the producer read every block. Folding the commands in would
// put the UI thread's error handling inside the type the RT path reads from, and the two have
// nothing to say to each other beyond these six values.
//
// SO WHY A TYPE AT ALL, if it has no methods. Because "the transport" is a thing, and until now it
// was six unrelated locals of main() that ten interfaces had to reassemble from parts. The type is
// what lets a module say it needs THE TRANSPORT rather than list which four of its six fields it
// happens to touch — and the difference shows up as 15 fewer members to keep in the right order.
//
// MEMBER NAMES ARE THE OLD LOCAL NAMES, as with HarmonyTimeline, so every reader moves unchanged
// and the diff is a move rather than a rewrite.
//
// ALL SIX ARE ATOMIC AND STAY ATOMIC. They are written by the command thread and read by the
// producer and the audio callback; grouping them in a struct changes nothing about that, and in
// particular does NOT make them consistent with each other — a reader that needs loopStart and
// loopEnd to agree still has to say so. effectiveLoop() in engine_rt_helpers.h is where that
// question is answered, and it remains the only place that answers it.
#include <atomic>
#include <cstdint>

namespace daw::engine {

struct TransportState {
  std::atomic<uint64_t> transportNanotick{0};
  std::atomic<bool> playing{false};
  std::atomic<uint64_t> loopStartNanotick{0};
  std::atomic<uint64_t> loopEndNanotick{0};
  std::atomic<bool> loopUserSet{false};
  std::atomic<uint64_t> transportElapsedNanotick{0};
};

}  // namespace daw::engine
