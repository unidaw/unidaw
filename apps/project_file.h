#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "apps/artifact_inventory.h"
#include "apps/device_chain.h"
#include "apps/stable_device_id.h"
#include "apps/harmony_timeline.h"
#include "apps/modulation.h"
#include "apps/automation_clip.h"
#include "apps/lane_quantize.h"
#include "apps/markers.h"
#include "apps/time_signature_map.h"
#include "apps/musical_structures.h"
#include "apps/patcher_graph.h"
#include "apps/track_routing.h"

namespace daw {

// Defined in apps/device_id_migration.h, which includes THIS header — so it is named here rather
// than included, and only by pointer.
struct DeviceIdMigration;

// The `kind` string a device serialises as. Declared here rather than left TU-local because
// diagnostics elsewhere need to NAME a device's kind, and a second switch over DeviceKind is
// the shape that goes stale one enum value at a time.
const char* deviceKindToString(DeviceKind kind);

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
// (decodeAudioFile) and mixed by the audio callback (renderAudioRegionBlock).
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

  friend bool operator==(const AudioClip&, const AudioClip&) = default;
  // AND != EXPLICITLY. This is built as C++17, where a defaulted operator== is a clang
  // extension that does NOT synthesise its negation — and std::vector's own operator==
  // reaches for element != in its constexpr path, so a vector of this type would fail to
  // compile with a message pointing deep inside libc++ rather than here.
  friend bool operator!=(const AudioClip& a, const AudioClip& b) { return !(a == b); }
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
  // EDIT SCOPE FOR THIS APPEARANCE (owner's call). When set, a note edit landing in this
  // placement is recorded as an override on IT rather than written to the clip — without the
  // caller having to say so each time.
  //
  // Chosen over a global "local edit mode" because of failure asymmetry, which is the whole
  // argument. Forget to set this and your hat appears in all three choruses: LOUD, visible
  // instantly, one undo away. Be in the wrong global mode and "fix the bass in chorus 1"
  // silently does NOT propagate: QUIET, nothing looks wrong, and you may not notice for an hour.
  // A loud failure beats a quiet one. It also puts the state on the thing it affects, next to
  // the override badge already on that placement, rather than in a mode you have to remember.
  //
  // The explicit per-command bit (kUiEditScopeLocal) still wins on its own — it is what daw-cli
  // uses and what makes scope testable without any UI state. Scope is never INFERRED from
  // whether the cell is occupied: that breaks the promise in one direction or the other
  // depending on the rule you pick, which is why an explicit signal exists at all.
  bool localEdits = false;
  // M2.57: THE OTHER VERSION OF THIS APPEARANCE. 0 = none.
  //
  // What plays is always `clipId` — there is no second fact about it and no "am I auditioning"
  // mode to get out of step with what you hear. An alternate is simply another clip this
  // placement can swap to, and SwapPlacementClip exchanges the two. That is the whole mechanism.
  //
  // It exists for the agent: an agent that writes straight into your clip leaves you undoing its
  // work, with its edits interleaved with yours in one undo stack and no way to hear the two
  // side by side. Instead it forks your clip, writes into the copy, and yours becomes the
  // alternate — so comparing is one command, keeping is doing nothing, and rejecting is one more
  // command. None of it touches undo, because none of it is a destructive edit.
  //
  // Persisted, so a draft survives closing the project. Written only when non-zero, so a project
  // that has never had an alternate stays byte-identical.
  uint32_t alternateClipId = 0;
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
  // SOUND-ADDRESSED ONLY (owner ruling, docs/SAMPLER_DESIGN.md section 8 Q2). Off by default, so
  // a blank `sound` means "the keymap picks the slot from pitch" — R5 stands. On, pitch NEVER
  // selects: the note's `sound` names the slot and pitch is varispeed relative to that slot's
  // rootKey, so a 64-slot kit stays fully chromatic instead of one slot per key.
  //
  // Per track rather than per device because it describes how THIS TRACK'S NOTES are read, not
  // what the sampler contains — the same kit dragged onto a second track can be played the other
  // way without copying anything.
  bool soundAddressedOnly = false;
  // CUT-ON-NEXT, OR LET IT RING (docs/TRACKER_GAP_LIST.md item 1). Off by default, so a project
  // written before this existed behaves exactly as it did: entering a note over a sounding one in
  // the same column TRUNCATES the sounding note.
  //
  // ON, the truncate is skipped and both notes keep their authored durations — an arpeggiated
  // chord or a ringing 808 down one column. The scheduler already plays that correctly; measured
  // over the overlap of two notes authored by hand, one alone is 3674 RMS and both together are
  // 5098, which is their power sum. Nothing in playback had to change.
  //
  // IT IS A DECISION NOT TO DESTROY DATA, which is why it belongs here rather than in the UI.
  // The old behaviour shortened the note IN THE DOCUMENT at entry, so the length the player
  // actually typed was gone and no later view could recover it.
  //
  // PER TRACK, not per note: a lane is either one where notes ring into each other or one where
  // they do not, and a per-note field for it would be 99% zeros.
  //
  // NOTE FOR ANYONE TESTING THIS: the sampler slot's own `nna` cuts the previous voice one layer
  // down and defaults to 0 = Cut. With a default kit, turning this on changes the DOCUMENT and
  // you still hear one note. Both have to say continue. That silent false negative is the whole
  // reason this comment exists.
  bool allowNoteOverlap = false;
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

  // A LEAF WITH NO INVARIANT compares memberwise. documentFieldsEqual needs every type it
  // reaches to answer either through a field list or through this; without one of the two,
  // documentFieldsEqual<ProjectDocument> does not COMPILE — which is how these seven were
  // found, because nothing had ever instantiated the comparer at the top level.
  friend bool operator==(const ProjectTempoPoint&, const ProjectTempoPoint&) = default;
  // AND != EXPLICITLY. This is built as C++17, where a defaulted operator== is a clang
  // extension that does NOT synthesise its negation — and std::vector's own operator==
  // reaches for element != in its constexpr path, so a vector of this type would fail to
  // compile with a message pointing deep inside libc++ rather than here.
  friend bool operator!=(const ProjectTempoPoint& a, const ProjectTempoPoint& b) { return !(a == b); }
};

struct ProjectMeta {
  std::string name = "Untitled";
  std::string createdUtc;
  std::string modifiedUtc;

  friend bool operator==(const ProjectMeta&, const ProjectMeta&) = default;
  // AND != EXPLICITLY. This is built as C++17, where a defaulted operator== is a clang
  // extension that does NOT synthesise its negation — and std::vector's own operator==
  // reaches for element != in its constexpr path, so a vector of this type would fail to
  // compile with a message pointing deep inside libc++ rather than here.
  friend bool operator!=(const ProjectMeta& a, const ProjectMeta& b) { return !(a == b); }
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
  // M3.28: NAMED POSITIONS. A flat list of (tick, name, colour) — "here is the chorus".
  // Replaces the section spine; apps/markers.h records why in full. Empty = a song with no
  // named structure, which is every project that has ever been written.
  std::vector<Marker> markers;
  // M3.28: THE SONG'S TIME-SIGNATURE MAP, and it is now AUTHORITATIVE rather than a derived
  // read-back of the spine. This is where mid-song meter lives: a 7/8 passage is a point in
  // this map, and TimeSignatureMap prefix-sums bars across it so "bar 9" means one thing.
  //
  // It was demoted to derived output when the meter moved onto the Section, for a stated reason
  // — "lengthening an earlier section moved every section and left the meter points behind." That
  // reason is void: a time edit now carries these points the same way it already carried the
  // tempo map, the harmony timeline, every placement and every automation point.
  //
  // Distinct from a CLIP's own meter (ProjectClip::timeSig*): a 7/8 clip draws its own accents
  // inside bars the song still numbers in the song's meter. Written only when it says something
  // a single signature could not, so a one-meter project stays byte-identical.
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
  // THE PROJECT'S DEVICE-ID HIGH-WATER MARK — the id the next allocation would take.
  //
  // AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME. Device ids are PROJECT-GLOBAL, and an id is never
  // reused: delete the only device on a track and the next one added gets a new number, not the
  // dead one's. That is what makes an id a durable name — automation, mirrors, plugin-state blobs,
  // parameter manifests, meters, modulation, sampler and patcher ownership all key on it across
  // save/load, and a reused id would silently hand a deleted device's state to its replacement.
  //
  // A WATERMARK RATHER THAN max(existing)+1 for exactly that reason: max+1 over the surviving
  // devices is what the old track-scoped allocator did, and it hands out a deleted id the moment
  // the highest-numbered device goes away.
  //
  // PERSISTED, because the guarantee is about the PROJECT and not about one session. It is one
  // value past the largest id in the document, so `kStableDeviceIdExhausted` (0x8000) means the
  // space is used up — and validateGlobalDeviceIds refuses any device id at or above it, which is
  // what stops a hand-edited file from claiming an id the next allocation would also hand out.
  //
  // NOT LOWERED BY UNDO. The document field travels with the version, but the engine's live
  // watermark only ever rises (DeviceIdWatermark::adopt takes the max), so stepping back over an
  // "add device" does not make its id available again to a later add.
  uint32_t nextDeviceId = kStableDeviceIdMin;
  // THE PROJECT'S PLUGIN ARTIFACTS, named by this document rather than found on disk.
  //
  // AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME. `artifactGeneration` is the SHA-256 of
  // `artifactEntries` in their sorted order, and the files live under
  // `<state dir>/generations/<artifactGeneration>/`. Load resolves ONLY these entries and verifies
  // every byte; it never enumerates the state directory, which is what makes a stale
  // canonical-looking file unreachable instead of merely unlikely. See apps/artifact_inventory.h
  // for why locating a blob by guessing its filename was provenance by coincidence.
  //
  // An EMPTY inventory still has a generation — the digest of the canonical empty list — so "this
  // project has no plugin state" is a value rather than a missing field.
  //
  // DEFAULTS TO THE EMPTY GENERATION, not to an empty string. A default-constructed document is a
  // project with no hosted plugins, which HAS an inventory — an empty one — and must validate
  // without every producer remembering to stamp it. An empty string would make the common case
  // the invalid one.
  std::string artifactGeneration = artifactEmptyGenerationId();
  std::vector<ArtifactEntry> artifactEntries;
};

// Schema version written into project.json. Bump on any incompatible change.
uint32_t projectSchemaVersion();

// Serializes to canonical JSON: fixed key order, real numeric types, one
// record per line where practical, so successive saves of an unchanged
// document are byte-identical and a musical change reads as a small diff.
std::string serializeProject(const ProjectDocument& document);

// `migration` receives the schema 1-5 -> project-global device-id rewrite that this parse
// performed, when one was needed. It is an OUTPUT rather than a side effect because the plugin
// state blob and parameter manifest of a migrated device still live under its OLD
// {trackId, oldDeviceId} name on disk, and only this map says what that name was — see
// DeviceIdMigration::legacyArtifactKeys. A schema-6 document needs no rewrite and leaves
// `migrated` false.
bool deserializeProject(const std::string& json,
                        ProjectDocument& document,
                        std::string* error = nullptr,
                        DeviceIdMigration* migration = nullptr);

// Writes atomically: a temp file beside the target, then rename, so an
// interrupted save can never truncate an existing project.

// WHERE OPAQUE PLUGIN STATE LIVES, derived from the document's own path: `<dir>/<stem>.state/`,
// holding one `t<track>_d<device>.bin` blob per device plus its `.params.json` manifest.
//
// ONE DEFINITION, because THREE places have to agree on it and until now only two could see it.
// It was a lambda inside daw_engine_main.cpp, so the save that writes the blobs and the load that
// restores them agreed — and saveProjectModule, which has to FIND those blobs in order to pack
// them, could not name the directory at all. That is the whole reason a `.uni` travelled with
// every sample and not one plugin's sound: the packer was not ignoring plugin state, it had no
// way to ask where it was.
std::string pluginStateDirFor(const std::string& projectPath);

// ---------------------------------------------------------------------------------------------
// THE `.uni` MODULE (docs/SAMPLER_DESIGN.md R3).
//
// "The project is a module, and it is called .uni" — a zip holding project.json plus a samples/
// directory, exactly as MOD, XM, IT, Renoise and Live all do it. Broken sample links stop
// existing, and sending someone a song is sending them one file.
//
// THE LOOSE DIRECTORY IS NOT REPLACED. The two forms are the same document at two levels of
// packing: a directory you edit and diff while working, a file you send. Packing rewrites every
// asset path to `samples/NAME`; unpacking writes them back out beside a project.json, so the
// unpacked form is an ordinary project the rest of the program already knows how to open.
//
// `assetBaseDir` resolves the document's relative asset paths when packing. `unpackDir` is where
// unpacking puts them.
//
// `stateBaseDir` is the loose project's plugin-state directory — pluginStateDirFor(loosePath).
// Empty, or a directory that does not exist, means the project has no plugin state, which is not
// an error: a project with no hosted plugins has none. An UNREADABLE file IS refused, for the
// same reason a missing sample is — a module that quietly drops a plugin's state is only found
// out after it has been sent.
bool saveProjectModule(const ProjectDocument& document,
                       const std::string& modulePath,
                       const std::string& assetBaseDir,
                       const std::string& stateBaseDir,
                       std::string* error = nullptr);

bool loadProjectModule(ProjectDocument& document,
                       const std::string& modulePath,
                       const std::string& unpackDir,
                       std::string* error = nullptr);

bool saveProject(const ProjectDocument& document,
                 const std::string& path,
                 std::string* error = nullptr);
// `migration` is the same output deserializeProject produces — see there. A caller that restores
// plugin state MUST take it: a device whose id the migration changed still has its state blob and
// parameter manifest on disk under the OLD {trackId, oldDeviceId} name, and only this map says
// what that name was.
bool loadProject(ProjectDocument& document,
                 const std::string& path,
                 std::string* error = nullptr,
                 DeviceIdMigration* migration = nullptr);

}  // namespace daw
