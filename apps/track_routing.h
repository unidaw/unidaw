#pragma once

#include <cstdint>

namespace daw {

enum class TrackRouteKind : uint8_t {
  None = 0,
  Master = 1,
  Track = 2,
  ExternalInput = 3,
};

struct TrackRoute {
  TrackRouteKind kind = TrackRouteKind::None;
  uint32_t trackId = 0;
  uint32_t inputId = 0;
};

struct TrackRouting {
  TrackRoute midiIn{};
  TrackRoute midiOut{};
  TrackRoute audioIn{};
  TrackRoute audioOut{TrackRouteKind::Master, 0, 0};
  // Movement 4 sidechain: where this track's sidechain (key) input comes from. Kind
  // Track = another track's output feeds the first chain plugin's sidechain input bus
  // (e.g. a compressor keyed off the kick). None = no sidechain. The source's output
  // is pulled one block late, which a dynamics processor's attack absorbs.
  TrackRoute sidechain{};
  bool preFaderSend = true;
};

inline TrackRouting defaultTrackRouting() {
  return TrackRouting{};
}

}  // namespace daw
