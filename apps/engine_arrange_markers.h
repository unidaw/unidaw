#pragma once

// THE MARKER LIST AND THE ARRANGEMENT VERSION.
//
// The fifth engine object of #26. markerList, arrangeVersion and arrangeMutex appeared as FOURTEEN
// members across five *Deps structs, and this is the cleanest grouping so far: four of the five
// want all three, and the fifth (SaveProjectDeps) wants two.
//
// A KNOWN IMPERFECTION, STATED RATHER THAN IMPLIED, and it is about the mutex.
//
// arrangeMutex does NOT only guard markerList. Reading every lock site, it also guards
// SongTiming::songMeter and SongTiming::loadedTempoMap:
//
//     std::lock_guard<std::mutex> lock(arrangeMutex);
//     document.markers   = markerList.markers();
//     document.timeSigMap = songTiming.songMeter.points();
//     s.tempoMap          = songTiming.loadedTempoMap;
//
// So the object boundary here does not match the LOCK boundary, and "guarded by the same mutex" is
// close to the strongest statement available that two pieces of state belong together. By that
// measure markerList, songMeter and loadedTempoMap should be ONE object, and SongTiming — created
// two commits earlier — should hold only meterSnapshot and the song-level scalars.
//
// It is left as it is for now, deliberately, for one reason worth writing down: TransportCommandDeps
// wants loadedTempoMap and NOTHING else in that group. Merging on the lock would hand it four
// members it never touches, which is the rule the last three objects were split on. The lock says
// merge; the usage says split; they disagree, and pretending otherwise by picking the answer that
// scores better would be the worse outcome.
//
// WHAT THAT MEANS FOR A READER: taking this object does not give you the whole thing arrangeMutex
// protects. Code that holds arrangeMutex while touching the meter or the tempo map is correct and
// will not look it from here. That is a real cost of this split and the reason it is written at the
// top of the file rather than in a commit message nobody will read again.
//
// MEMBER NAMES ARE THE OLD LOCAL NAMES, so every reader moves unchanged.
#include <atomic>
#include <cstdint>
#include <mutex>

#include "markers.h"

namespace daw::engine {

struct ArrangeMarkers {
  daw::MarkerList markerList;
  // Bumped whenever anything on the arrangement rail changes — markers, and the meter points that
  // live in SongTiming. A reader that watches this to know "the ruler moved" is watching the right
  // number even though half of what it covers is not in this struct.
  std::atomic<uint32_t> arrangeVersion{0};
  std::mutex arrangeMutex;
};

}  // namespace daw::engine
