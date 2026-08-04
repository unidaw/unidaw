#pragma once

// THE TRACK TABLE — every track the engine is running, and the mutex that guards the vector.
//
// The sixth engine object of #26, and the one with the least to argue about. `tracks` and
// `tracksMutex` appear in NINETEEN *Deps structs each — thirty-eight members — and they are
// PERFECTLY CORRELATED: every struct that takes one takes the other, and neither has ever appeared
// without the other. There is no judgement call here about what belongs together, because the tree
// has been saying it in every interface for as long as the interfaces have existed.
//
// It is also the reason the free functions look the way they do. trackAt(tracks, tracksMutex, id)
// takes both, always, because it cannot do its job with either alone — a signature that has been
// carrying this type around without a name for it.
//
// WHAT IS NOT HERE: masterTrack. It appears in seven structs, and not all of them take `tracks` —
// so including it would hand fourteen structs a master track they never touch, and would also be
// wrong on its own terms. The master is not an entry in this table: it has no arrangement rail, no
// clips and no tracker lane, and every place that walks `tracks` deliberately does not walk it.
// Putting it in the container it is defined by not being in would be a poor kind of tidiness.
//
// THE MUTEX GUARDS THE VECTOR, NOT THE TRACKS. It serialises adding, removing and iterating the
// table; each TrackRuntime has its own trackMutex for its own contents. That distinction is why
// trackAt() can return a pointer and let the caller lock the track it found, and grouping these two
// does not change it. Same caveat as everywhere else in #26: a struct is not a lock.
//
// MEMBER NAMES ARE THE OLD LOCAL NAMES, so every reader moves unchanged.
#include <memory>
#include <mutex>
#include <vector>

#include "engine_types.h"

namespace daw::engine {

struct TrackTable {
  std::vector<std::unique_ptr<TrackRuntime>> tracks;
  std::mutex tracksMutex;
};

}  // namespace daw::engine
