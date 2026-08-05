#pragma once

// THE ARRANGEMENT RAIL: the markers, the meter map, the version that covers both, and the one
// lock that makes them consistent.
//
// The fifth engine object of #26, and the boundary is now the LOCK's. markerList, arrangeVersion
// and arrangeMutex appeared as fourteen members across five *Deps structs; songMeter joined them
// once the lock was actually read rather than summarised.
//
// WHY THE METER IS IN HERE. Every access to songMeter in the engine is under arrangeMutex —
// all twenty of them, across the consumer's publish, both save paths, the load, the store
// snapshot and the arrange-time commands. "Guarded by the same mutex" is close to the strongest
// statement available that two pieces of state belong together, and here it is unanimous. It also
// matches what arrangeVersion has always claimed to cover: markers AND meter.
//
// THIS FILE USED TO SAY THE MERGE COULDN'T HAPPEN, and the reason was wrong. It claimed
// arrangeMutex also guarded SongTiming::loadedTempoMap, and that merging on the lock would
// therefore hand TransportCommandDeps four members it never touches. Reading the two sites it
// quoted:
//
//     std::lock_guard<std::mutex> alock(arrangeMutex);
//     s.markers     = arrange.markerList.markers();
//     s.meterPoints = songTiming.songMeter.points();
//     }                                    // <-- the lock ends HERE
//     s.tempoMap    = songTiming.loadedTempoMap;
//
// loadedTempoMap is OUTSIDE the lock at both of them, and is documented as single-threaded on the
// UI command thread. The objection was a misquote of a closing brace. Nothing wanted the four
// members, so nothing was traded away — see the memory on pinned defects overestimating their fix.
//
// WHAT STAYED BEHIND, and why it is not the same mistake. meterSnapshot remains in SongTiming: it
// is the immutable copy the PRODUCER reads with an atomic load and no lock at all, which is the
// whole point of it. The writers below happen to publish it inside their critical section because
// it is derived from songMeter there, but it is an atomic handoff rather than lock-guarded state,
// and the producer must not be handed a marker list to get at it.
//
// MEMBER NAMES ARE THE OLD LOCAL NAMES, so every reader moves unchanged.
#include <atomic>
#include <cstdint>
#include <mutex>

#include "markers.h"
#include "time_signature_map.h"

namespace daw::engine {

struct ArrangeRail {
  daw::MarkerList markerList;
  // The song's meter map. Mutable, and every read and write of it is under arrangeMutex — which
  // is why it lives here and not with the snapshot the RT reads.
  daw::TimeSignatureMap songMeter;
  // Bumped whenever anything on the arrangement rail changes: markers or meter. Both are now in
  // this struct, so a reader watching this number is watching state it can also reach.
  std::atomic<uint32_t> arrangeVersion{0};
  std::mutex arrangeMutex;
};

}  // namespace daw::engine
