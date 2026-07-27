#pragma once

#include <cstdint>
#include <optional>
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

// What kind of material a clip holds. The format carries this slot from the
// start so audio regions are representable before it freezes — the retrofit that
// symbolic-only formats never survive (ARCHITECTURE_REVIEW §7, Movement 4). The
// engine plays Symbolic today; Audio clips persist and round-trip but are not yet
// scheduled (their playback lands with the Movement 4 audio engine).
enum class ClipKind : uint8_t {
  Symbolic = 0,  // notes/chords in `clip`
  Audio = 1,     // a region of an audio source, described by `audio`
};

// An audio region: a window into a source file placed on the timeline. Times on
// the timeline (the clip's lengthNanoticks, the fades) are musical nanoticks; the
// source in-point is in sample frames, the source's own unit, resolved against
// the file's rate at play time. Minimal by design — enough to represent and
// round-trip a region; warp/stretch, multi-channel, and clip-gain automation are
// additive within this variant later.
struct AudioClip {
  std::string sourcePath;          // the audio file (durable source ids can come later)
  uint64_t sourceStartFrame = 0;   // in-point into the source, in sample frames
  double gainDb = 0.0;
  uint64_t fadeInNanoticks = 0;
  uint64_t fadeOutNanoticks = 0;
};

// A reusable clip definition. Project-level (Song.clips), referenced by
// placements on any track. For a Symbolic clip, events are CLIP-RELATIVE — 0-based
// within the clip, never absolute timeline ticks. `lengthNanoticks` is the clip's
// own extent / loop period; a placement longer than this loops the clip to fill
// itself. `kind` selects which payload is meaningful: `clip` for Symbolic, `audio`
// for Audio.
struct ProjectClip {
  uint32_t id = 0;
  std::string name;
  uint64_t lengthNanoticks = 0;
  // The clip's musical grid, owned per-clip (a clip is a "section"). lines_per_beat is
  // the tracker row subdivision; the time signature governs bar length within the
  // clip. Moved here from the track so each clip carries its own meter; the active
  // grid is the clip under the playhead, and a new clip inherits its predecessor's.
  // Meaningful for Symbolic clips.
  uint32_t linesPerBeat = 4;
  uint32_t timeSigNumerator = 4;
  uint32_t timeSigDenominator = 4;
  ClipKind kind = ClipKind::Symbolic;
  MusicalClip clip;   // meaningful when kind == Symbolic
  AudioClip audio;    // meaningful when kind == Audio
};

// A placement of a clip on a track. `at` is the absolute timeline tick where the
// clip's local tick 0 lands; nullopt = a loose session cell (ARCHITECTURE_REVIEW
// ruling (e)). `lengthNanoticks` is the placement's timeline extent (0 = one
// clip length); a shorter clip loops within [at, at+length). Overrides are
// additive-only, one level deep (ruling (d)): `adds` inserts notes local to this
// placement, `mutes` silences base notes by EventId. Resolved = base - mutes + adds.
struct ProjectPlacement {
  uint32_t clipId = 0;
  std::optional<uint64_t> at;
  uint64_t lengthNanoticks = 0;
  std::vector<MusicalEvent> adds;
  std::vector<EventId> mutes;
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
  // Placements of project-level clips on this track. Every note lives in a clip
  // reached through a placement — no notes outside clips.
  std::vector<ProjectPlacement> placements;
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
  // Project-level clip library, referenced by track placements by id.
  std::vector<ProjectClip> clips;
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
