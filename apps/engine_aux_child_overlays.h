#pragma once
// A LOADED CHILD TRACK'S MATERIAL, HELD UNTIL ITS BUS EXISTS.
//
// A multi-out instrument's stems are child tracks DERIVED from the parent's bus layout, so at load
// time the children in the file have no runtime to be applied to yet — the parent's host has not
// reported its buses. The overlay holds each child's name, mixer, placements, clips and automation
// until reconcileChildTracks places that bus, and is consumed when applied.
//
// KEYED BY (parent track id, BUS INDEX), and that is the rule worth knowing. NOT by track id: a
// child's id is assigned from the live track count when it is derived, so adding one document track
// renumbers every stem and material keyed by id would come back on the wrong lane. Consuming the
// entry on application is what makes application happen exactly once, and the whole map is cleared
// by the next load so a stem whose bus never comes back cannot leak into a different project.
//
// THE MUTEX AND THE MAP ARE ONE THING. The load writes it, the consumer drains it on its own tick,
// and every access to either is an access to both. They were two main() locals threaded through
// ConsumerDeps and LoadProjectDeps as two members each.
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

#include "engine_types.h"

namespace daw::engine {

struct AuxChildOverlays {
  std::map<std::pair<uint32_t, uint32_t>, AuxChildOverlay> auxChildOverlays;
  std::mutex auxChildOverlayMutex;
};

}  // namespace daw::engine
