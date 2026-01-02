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
  bool preFaderSend = true;
};

inline TrackRouting defaultTrackRouting() {
  return TrackRouting{};
}

}  // namespace daw
