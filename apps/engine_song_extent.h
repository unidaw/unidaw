#pragma once

// HOW LONG THE SONG IS, AND HOW FAR ONE TRACK REACHES.
//
// trackWindowEnd answers "where does this track's content stop" for a single track; recomputeSongEnd
// asks that of every track and publishes the maximum as the song's end. One is the other's inner
// loop, which is why they move together — extracting either alone would leave that call crossing a
// module boundary.
//
// THE SONG END IS NOT THE ARRANGEMENT'S LENGTH. It is where the last sounding thing finishes, which
// is what a render bounds itself by and what the ruler draws to. A placement that reaches past its
// clip, a loop that wraps, and a track with nothing on it all have to give the same answer they gave
// before, which is why both bodies moved verbatim rather than being rewritten around the new
// interface.
//
// FOUR DEPENDENCIES for 63 lines: the transport (for the loop bounds), the song timing (for the
// value it publishes), the track snapshot accessor, and the pattern length.
#include <cstdint>
#include <functional>
#include <vector>

#include "engine_song_timing.h"
#include "engine_transport_state.h"
#include "engine_types.h"

namespace daw::engine {

struct SongExtentDeps {
  TransportState& transport;
  SongTiming& songTiming;
  const std::function<std::vector<TrackRuntime*>()>& snapshotTracks;
  // The arrangement's own loop length in ticks, taken by value: it is a configuration constant for
  // the life of the engine, not live state, and a reference would suggest otherwise.
  const uint64_t patternTicks;
};

uint64_t trackWindowEnd(SongExtentDeps& deps, const TrackRuntime& rt);
void recomputeSongEnd(SongExtentDeps& deps);

}  // namespace daw::engine
