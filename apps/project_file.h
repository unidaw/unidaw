#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "apps/device_chain.h"
#include "apps/harmony_timeline.h"
#include "apps/modulation.h"
#include "apps/automation_clip.h"
#include "apps/lane_quantize.h"
#include "apps/section_list.h"
#include "apps/time_signature_map.h"
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
// engine plays both: Symbolic clips schedule notes, and Audio clips are decoded
// (decodeAudioFileMono) and mixed by the audio callback (renderAudioRegionBlock).
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
  // Stable, monotonic per-placement id (published in placementId; the arrangement's Move/
  // Resize/Remove key on it). 0 = unassigned (a pre-id file or a fresh placement); the
  // engine assigns one on creation and on load. It lives on the placement so it survives
  // edits and the undo store-swap, and is persisted so it is stable across save/load too.
  uint32_t id = 0;
  std::optional<uint64_t> at;
  uint64_t lengthNanoticks = 0;
  std::vector<MusicalEvent> adds;
  std::vector<EventId> mutes;
};

struct ProjectTrack {
  uint32_t trackId = 0;
  std::string name;
  // The MASTER track (patcher-is-a-device item 4): a device chain + mixer whose output
  // is the master bus, no clips/placements. Serialized in the tracks array with
  // "is_master": true and lifted out of it by the engine on load. Reuses ProjectTrack
  // purely for its chain/mixer/mod-link serialization.
  bool isMaster = false;
  // Movement 4 child-track structure: a child is a real track fed from a plugin
  // output bus instead of a clip. parentId 0 = top-level, else the parent track_id;
  // `collapsed` hides the children in the UI (a view filter, never a data change).
  uint32_t parentId = 0;
  bool collapsed = false;
  // An AUX CHILD — a lane the engine DERIVES, one per enabled aux output bus of the
  // parent's multi-out plugin. These used to be skipped by the save entirely, with a good
  // reason (written as a plain track, a child reloads as a top-level lane fed by nothing)
  // and a bad consequence: notes typed on a stem were accepted, they sounded, and they were
  // gone after a reload with nothing reporting a loss. So a child is now saved the way the
  // master track is — a FLAGGED entry, lifted out of `tracks` on load and reattached after
  // the children are re-derived.
  //
  // It reattaches by BUS INDEX, never by track id or list position. A child's id is
  // assigned from the live track count when it is derived, so it moves whenever the
  // document's track count changes; the bus it came from is the only stable name it has.
  // `auxBusIndex` is meaningful only when isAuxChild is set (bus 0 is the main output and
  // never becomes a child, so 0 doubles as "unset" without ambiguity).
  bool isAuxChild = false;
  uint32_t auxBusIndex = 0;
  bool harmonyQuantize = false;
  // M3.27: this track's automation. Playback existed since Movement 3 phase 1 and this
  // did NOT, so automation could be heard and never saved — record a sweep, hear it,
  // reload, and it was gone. Written only when non-empty, so a project without automation
  // is byte-identical to what it was.
  std::vector<AutomationClip> automationClips;
  // Rows one beat is cut into for this lane's tracker grid (Mock B per-lane
  // grids): 4 = 16ths, 3 = triplets. The engine persists this but does not use
  // it — note timing is stored in nanoticks and is grid-independent.
  uint32_t linesPerBeat = 4;
  // M1.13: non-destructive quantize for this lane. It changes where notes SOUND, never
  // what is stored, so it lives beside linesPerBeat as a lane attribute rather than
  // anywhere near the notes. Default is off, and off is the identity.
  LaneQuantize quantize{};
  MixerSettings mixer{};
  TrackRouting routing{};
  TrackChain chain{};
  std::vector<ModLink> modLinks;
  // Placements of project-level clips on this track. Every note lives in a clip
  // reached through a placement — no notes outside clips.
  std::vector<ProjectPlacement> placements;
  // No track-level patcher: a patcher is a DEVICE (in `chain`), so it has a
  // position in the signal path. A legacy schema-<=3 track-level "patcher" is
  // migrated into a head-of-chain PatcherEvent device on load (see
  // deserializeProject); it is never a field here and never re-serialized.
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
  // The SONG's time signature — what the arrangement ruler and the tracker's time
  // gutter count in. Separate from a clip's own meter (ProjectClip time_sig_*): a
  // 7/8 clip draws its own accents inside bars that the song still numbers in its
  // meter, so polymetric clips never lose a shared sense of where you are.
  // M3.22: the SONG's time-signature MAP. songTimeSig* below is the map's FIRST entry,
  // kept as its own field so every existing reader and every file written before this
  // still means what it meant — a project with one signature writes an empty map and
  // reads back identically. A non-empty map supersedes it.
  // M3.23: the section spine — the ORDERED list of named spans that is the arrangement.
  // Each entry stores a bar COUNT, never a start position: "chorus 1 is at bar 9" is a
  // consequence of the intro being 8 bars, so lengthening the intro moves everything
  // after it and no two facts about the same position can disagree. Empty = a song with
  // no named structure, which is every project written before this field.
  std::vector<Section> sections;
  std::vector<TimeSignaturePoint> timeSigMap;
  uint32_t songTimeSigNumerator = 4;
  uint32_t songTimeSigDenominator = 4;
  // Generation seed. Every patcher generator folds this into its hash, so a song's
  // generated material reproduces exactly across loads — and changing this one number
  // re-rolls every variation while everything authored stays put. 0 = unseeded.
  uint64_t seed = 0;
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
