#pragma once
// TELLING THE UI WHAT THE CLIPS ARE — two publishers lifted verbatim out of main().
//
// writeUiClipExtents publishes where every clip starts and ends, gated on the clip version so a
// quiet project costs nothing. publishAudioClipTable publishes the audio clips proper: resolved
// paths, decoded waveform handles, tempo-mapped extents. Both write into the same uiShm block and
// both are the engine talking outward, which is why they share a file.
//
// TWO STRUCTS, NOT ONE, and the reason is declaration order rather than design. resolveSourcePath
// is declared BETWEEN them in main(), so a single struct could not be constructed before the
// first publisher without naming something that does not exist yet. See docs/PROGRESS.md — that
// constraint, not coupling, is what makes a single enormous scope hard to break up.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "engine_track_table.h"
#include "engine_publish_gates.h"
#include "engine_types.h"
#include "project_file.h"
#include "time_base.h"
#include "waveform_store.h"

namespace daw::engine {

struct ClipExtentsDeps {
  // Nine publish gates in one: see apps/engine_publish_gates.h.
  PublishGates& publishGates;
  std::atomic<uint32_t>& clipVersion;
  std::function<std::vector<TrackRuntime*>()> snapshotTracks;
  UiShmState& uiShm;
};

struct AudioClipTableDeps {
  std::vector<daw::ProjectClip>& loadedClips;
  std::mutex& loadedClipsMutex;
  std::function<std::string(const std::string&)> resolveSourcePath;
  daw::TempoMapProvider& tempoProvider;
  TrackTable& trackTable;
  UiShmState& uiShm;
  daw::WaveformStore& waveformStore;
};

// Publishes every clip's start and end. `force` republishes even when the version has not moved.
void writeUiClipExtents(ClipExtentsDeps& deps, bool force);

// Publishes the audio clip table: resolved paths, waveform handles, tempo-mapped extents.
void publishAudioClipTable(AudioClipTableDeps& deps);

}  // namespace daw::engine
