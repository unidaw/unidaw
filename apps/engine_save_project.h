// WRITING A PROJECT OUT, in a file of its own.
//
// 480 lines that lived inside main(), which meant the one function that decides what a saved
// project CONTAINS could not be read without scrolling through the audio engine, and could not be
// called from anywhere else.
//
// The capture list was enumerated by the compiler — `[&]` replaced with `[]`, and every "cannot
// be implicitly captured" error read off. Twenty names, of which only two are lambdas. That ratio
// is why this one moved and processTrack did not: processTrack captures nine lambdas and would
// have become a struct of callbacks on the producer path, which buys line count by making the
// design worse.
//
// OFF THE REAL-TIME PATH, so std::function here costs nothing that matters — this runs when a
// person saves, not once per audio block.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine_loaded_project.h"
#include "engine_track_table.h"
#include "engine_arrange_markers.h"
#include "engine_patcher_graph_owner.h"
#include "engine_song_timing.h"
#include "engine_harmony_timeline.h"
#include "apps/engine_types.h"
#include "apps/musical_structures.h"
#include "apps/note_entry.h"
#include "apps/patcher_graph.h"
#include "apps/plugin_cache.h"
#include "apps/project_file.h"
#include "apps/time_signature_map.h"

namespace daw::engine {

struct SaveProjectDeps {
  // What a load left behind: see apps/engine_loaded_project.h.
  LoadedProject& loadedProject;
  ArrangeMarkers& arrange;
  HarmonyTimeline& harmonyTimeline;
  std::atomic<uint32_t>& liveTrackCount;
  SongTiming& songTiming;
  std::unique_ptr<TrackRuntime>& masterTrack;
  PatcherGraphOwner& patcherGraph;
  const daw::PluginCache& pluginCache;
  std::atomic<uint64_t>& projectSeed;
  TrackTable& trackTable;
  std::function<daw::BarGrid()> songBarGrid;
  std::function<bool(const TrackRuntime&)> trackIsPersisted;
};

// Serialises the live engine state to `path`. Returns false and fills `error` on failure.
//
// READS LIVE STATE, NOT THE LOADED DOCUMENT, which is the property the module boundary now makes
// visible: what gets written is what the engine currently holds, so a save after an edit that
// never reached the document would silently write the old thing.
bool saveProjectToPath(SaveProjectDeps& deps, const std::string& path, std::string* error);

}  // namespace daw::engine
