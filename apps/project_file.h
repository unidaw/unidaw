#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apps/device_chain.h"
#include "apps/harmony_timeline.h"
#include "apps/modulation.h"
#include "apps/musical_structures.h"
#include "apps/track_routing.h"

namespace daw {

// Serializable form of a track. This mirrors the engine's `Track` but holds
// only what belongs in a saved document: no runtime state, no host handles.
struct ProjectTrack {
  uint32_t trackId = 0;
  std::string name;
  bool harmonyQuantize = true;
  TrackRouting routing{};
  TrackChain chain{};
  std::vector<ModLink> modLinks;
  MusicalClip clip;
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
// reconstruct a session, minus plugin state blobs and patcher graphs, which
// live beside project.json in the container (see PROJECT_PERSISTENCE.md).
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
