#pragma once

// THE UNDERRUN REPORTER THREAD.
//
// Wakes twice a second, and every ~2s prints how many dropout callbacks the audio device has taken
// since it last looked. It is the only thing in the engine that turns the starve counter into
// something a human reads without asking for it.
//
// Extracted from main() as a thread BODY, following runConsumerThread: main() should say which
// threads exist and what state they are given, not hold their bodies. The five inline thread
// bodies in main() were 677 lines between them.
//
// Body moved VERBATIM and diffed against the lambda it came from.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "engine_transport_state.h"
#include "engine_audio_callback.h"

namespace daw::engine {

// LIFETIME: THIS MUST OUTLIVE THE THREAD, and the scope to declare it in is the one the JOIN is
// in, not the one the std::thread construction is in. In main() the reporter is created inside an
// `if (!testMode && audioCallback)` block and joined ~180 lines below it, so a deps struct scoped
// to that block is destroyed while the thread still holds a reference to it.
//
// That was not a hypothetical — it was the first version of this extraction. The symptom was not a
// crash in the reporter: it was EIGHT UNRELATED CHECKS failing with "Failed to receive control
// header", which reads as a host-process fault in a different subsystem entirely. A struct of
// references gives you no warning about this; the compiler is happy and the damage lands somewhere
// else.
struct XrunReporterDeps {
  std::atomic<bool>& running;
  std::unique_ptr<EngineAudioCallback>& audioCallback;
  TransportState& transport;
  std::atomic<uint32_t>& nextBlockId;
  std::atomic<uint32_t>& audioPlaybackBlockId;
  std::atomic<uint32_t>& observedPipelineBlocks;
};

// blockMs and latencyReport were captured BY VALUE by the lambda — they are a snapshot of the
// device configuration taken when the thread started, not live state — so they stay parameters
// rather than becoming references in the deps struct. Passing them by reference would change what
// the thread reads if the device were ever reconfigured under it.
void runXrunReporter(XrunReporterDeps& deps, double blockMs, bool latencyReport);

}  // namespace daw::engine
