// THE PRODUCER'S FILE-SCOPE HELPERS, out of daw_engine_main.cpp.
//
// These three sat at file scope in main.cpp, which meant renderTrack could call them and nothing
// else could. That is the quiet mechanism by which a 15,000-line main() stays 15,000 lines: a
// helper written next to its only caller is invisible to every other file, so the next caller has
// to be written next to it too.
//
// Moved VERBATIM — they reference nothing from any enclosing scope.
#pragma once

#include <cstdint>
#include <vector>

#include "apps/engine_types.h"
#include "apps/patcher_graph.h"

namespace daw::engine {

constexpr uint32_t kPatcherScratchpadCapacity = 1024;

inline void dispatchRustKernel(daw::PatcherNodeType type, daw::PatcherContext& ctx) {
  switch (type) {
    case daw::PatcherNodeType::RustKernel:
      if (daw::patcher_process) {
        daw::patcher_process(&ctx);
      }
      break;
    case daw::PatcherNodeType::Euclidean:
      if (daw::patcher_process_euclidean) {
        daw::patcher_process_euclidean(&ctx);
      } else if (daw::patcher_process) {
        daw::patcher_process(&ctx);
      }
      break;
    case daw::PatcherNodeType::RandomDegree:
      if (daw::patcher_process_random_degree) {
        daw::patcher_process_random_degree(&ctx);
      } else if (daw::patcher_process) {
        daw::patcher_process(&ctx);
      }
      break;
    case daw::PatcherNodeType::SliceSelect:
      if (daw::patcher_process_slice_select) {
        daw::patcher_process_slice_select(&ctx);
      }
      // NO FALLBACK to the generic kernel, unlike RandomDegree above. The generic kernel does
      // something else entirely; for a node whose whole job is to write one field, running the
      // wrong kernel would silently produce notes with no slice rather than nothing at all —
      // and "the wrong sound plays" is harder to notice than "no sound plays".
      break;
    case daw::PatcherNodeType::EventOut:
      if (daw::patcher_process_event_out) {
        daw::patcher_process_event_out(&ctx);
      }
      break;
    case daw::PatcherNodeType::Passthrough:
      if (daw::patcher_process_passthrough) {
        daw::patcher_process_passthrough(&ctx);
      }
      break;
    case daw::PatcherNodeType::AudioPassthrough:
      if (daw::patcher_process_audio_passthrough) {
        daw::patcher_process_audio_passthrough(&ctx);
      }
      break;
    case daw::PatcherNodeType::Lfo:
      if (daw::patcher_process_lfo) {
        daw::patcher_process_lfo(&ctx);
      } else if (daw::patcher_process) {
        daw::patcher_process(&ctx);
      }
      break;
  }
}

inline void getClipEventsInRange(const ClipSnapshot& snapshot,
                                 uint64_t startTick,
                                 uint64_t endTick,
                                 std::vector<const daw::MusicalEvent*>& out) {
  out.clear();
  const auto& events = snapshot.events;
  auto it = std::lower_bound(
      events.begin(), events.end(), startTick,
      [](const daw::MusicalEvent& lhs, uint64_t tick) {
        return lhs.nanotickOffset < tick;
      });
  for (; it != events.end() && it->nanotickOffset < endTick; ++it) {
    out.push_back(&*it);
  }
}

}  // namespace daw::engine
