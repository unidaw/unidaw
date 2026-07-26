#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apps/device_chain.h"
#include "apps/harmony_timeline.h"
#include "apps/modulation.h"
#include "apps/musical_structures.h"
#include "apps/patcher_graph.h"
#include "apps/track_routing.h"

namespace daw {

// Serializable form of a track. This mirrors the engine's `Track` but holds
// only what belongs in a saved document: no runtime state, no host handles.
struct MixerSettings {
  double gainDb = 0.0;
  double pan = 0.0;  // -1 hard left, +1 hard right
  bool mute = false;
  bool solo = false;
};

struct ProjectTrack {
  uint32_t trackId = 0;
  std::string name;
  bool harmonyQuantize = false;
  // Rows one beat is cut into for this lane's tracker grid (Mock B per-lane
  // grids): 4 = 16ths, 3 = triplets. The engine persists this but does not use
  // it — note timing is stored in nanoticks and is grid-independent.
  uint32_t linesPerBeat = 4;
  MixerSettings mixer{};
  TrackRouting routing{};
  TrackChain chain{};
  std::vector<ModLink> modLinks;
  MusicalClip clip;
  // This track's patcher DAG. One patcher per track; an empty graph = none.
  PatcherGraph patcher;
};

struct ProjectTempoPoint {
  uint64_t nanotick = 0;
  double bpm = 120.0;
};

struct ProjectMeta {
  std::string name = "Untitled";
  std::string createdUtc;
  std::string modifiedUtc;
};

// The authoritative, recallable document. Everything the engine needs to
// reconstruct a session, minus plugin state blobs, which live beside
// project.json in the container (see PROJECT_PERSISTENCE.md). Each track carries
// its own patcher DAG, so a multi-patcher song is a set of per-track graphs.
struct ProjectDocument {
  ProjectMeta meta{};
  uint64_t nanoticksPerQuarter = 960000;
  std::vector<ProjectTempoPoint> tempoMap{{0, 120.0}};
  std::vector<HarmonyEvent> harmonyTimeline;
  std::vector<ProjectTrack> tracks;
};

// Schema version written into project.json. Bump on any incompatible change.
uint32_t projectSchemaVersion();

// Serializes to canonical JSON: fixed key order, real numeric types, one
// record per line where practical, so successive saves of an unchanged
// document are byte-identical and a musical change reads as a small diff.
std::string serializeProject(const ProjectDocument& document);
bool deserializeProject(const std::string& json,
                        ProjectDocument& document,
                        std::string* error = nullptr);

// Writes atomically: a temp file beside the target, then rename, so an
// interrupted save can never truncate an existing project.
bool saveProject(const ProjectDocument& document,
                 const std::string& path,
                 std::string* error = nullptr);
bool loadProject(ProjectDocument& document,
                 const std::string& path,
                 std::string* error = nullptr);

}  // namespace daw
