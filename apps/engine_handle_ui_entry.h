#pragma once
// THE UI COMMAND DISPATCHER, lifted out of main() as one verbatim block.
//
// It was a 1,625-line lambda inside an 11,666-line main(), which is the single largest
// reason a maintainability panel graded this file C. Nothing about the code changed in the
// move: the body is byte-for-byte what it was, and the 71 values it used to capture by
// reference arrive in HandleUiEntryDeps and are re-bound to their original names at the top
// of the function. That keeps the diff reviewable — a reader can diff the body against the
// old main() and see no edits — and it makes the dependency surface a written-down fact
// rather than an implicit [&].
//
// SEVENTY-ONE IS THE POINT, NOT AN EMBARRASSMENT. That number was invisible while it was an
// [&]; now it is a list you have to look at, and sixteen of the entries are themselves the
// per-module *CommandDeps structs, so the real shape is "16 modules plus the song-level
// state they share". Shrinking it is the next refactor, and it is now a measurable one.
//
// WIRED BY POSITION, CHECKED BY NAME: this struct is aggregate-initialised, so two adjacent
// members of the same type could be exchanged without the compiler noticing — and it has
// runs of eight std::atomic<uint32_t>&. tools/deps_order_check.sh asserts that the Nth
// argument at the call site is named after the Nth member, for this struct and the other
// seventeen.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine_automation_commands.h"
#include "engine_chain_commands.h"
#include "engine_clip_commands.h"
#include "engine_device_commands.h"
#include "engine_marker_commands.h"
#include "engine_modlink_commands.h"
#include "engine_module_commands.h"
#include "engine_note_commands.h"
#include "engine_patcher_commands.h"
#include "engine_project_commands.h"
#include "engine_request_commands.h"
#include "engine_rowops_commands.h"
#include "engine_sampler_commands.h"
#include "engine_trackprops_commands.h"
#include "engine_track_commands.h"
#include "engine_types.h"
#include "engine_undo_commands.h"
#include "event_log.h"
#include "markers.h"
#include "time_base.h"
#include "time_signature_map.h"

namespace daw::engine {

struct HandleUiEntryDeps {
  const std::function<bool(uint32_t, const std::function<bool(std::vector<daw::ProjectPlacement>&)>&)>& applyPlacementEdit;
  std::mutex& arrangeMutex;
  std::atomic<uint32_t>& arrangeVersion;
  AutomationCommandDeps& automationCommandDeps;
  std::atomic<uint32_t>& automationVersion;
  const std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)>& buildTrackSnapshot;
  std::vector<BulkStream>& bulkStreams;
  uint64_t& bulkTick;
  const std::function<uint32_t(TrackRuntime*)>& bumpClipVersionFor;
  ChainCommandDeps& chainCommandDeps;
  ClipCommandDeps& clipCommandDeps;
  std::atomic<bool>& clipDirty;
  std::atomic<uint32_t>& clipVersion;
  DeviceCommandDeps& deviceCommandDeps;
  const std::function<void(uint32_t, uint8_t, uint8_t, bool)>& enqueuePreview;
  const std::function<void(const std::vector<uint8_t>&)>& handleAssembledBulk;
  std::atomic<bool>& harmonyDirty;
  std::vector<daw::HarmonyEvent>& harmonyEvents;
  std::mutex& harmonyMutex;
  std::atomic<uint32_t>& harmonyVersion;
  std::unordered_map<uint32_t, std::vector<uint8_t>>& heldPreview;
  const std::function<void(const char*, const char*, uint32_t, uint32_t, const std::string&)>& historyAppend;
  std::atomic<uint32_t>& liveTrackCount;
  std::vector<daw::ProjectTempoPoint>& loadedTempoMap;
  std::atomic<uint64_t>& loopEndNanotick;
  std::atomic<uint64_t>& loopStartNanotick;
  std::atomic<bool>& loopUserSet;
  MarkerCommandDeps& markerCommandDeps;
  daw::MarkerList& markerList;
  std::unique_ptr<TrackRuntime>& masterTrack;
  std::shared_ptr<const daw::TimeSignatureMap>& meterSnapshot;
  ModlinkCommandDeps& modlinkCommandDeps;
  ModuleCommandDeps& moduleCommandDeps;
  std::atomic<uint32_t>& nextClipId;
  std::atomic<uint32_t>& nextPlacementId;
  NoteCommandDeps& noteCommandDeps;
  std::atomic<bool>& panicPending;
  PatcherCommandDeps& patcherCommandDeps;
  const uint64_t patternTicks;
  std::vector<PreviewNoteReq>& pendingPreviewNotes;
  std::atomic<bool>& playing;
  std::mutex& previewMutex;
  ProjectCommandDeps& projectCommandDeps;
  const std::function<void(uint32_t, TrackStoreState, TrackStoreState)>& pushStructuralUndo;
  const std::function<void(EngineUndoEntry)>& pushUndo;
  const std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)>& rebuildAudioRender;
  const std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)>& rebuildFlatAndPublish;
  const std::function<void()>& recomputeSongEnd;
  RequestCommandDeps& requestCommandDeps;
  const std::function<bool(uint32_t, daw::UiCommandType, uint32_t)>& requireMatchingClipVersion;
  std::atomic<bool>& resetTimeline;
  const std::function<void(TrackRuntime&)>& resetTrackContent;
  std::condition_variable& restartCv;
  const std::function<bool(TrackRuntime&, const std::vector<std::string>&)>& restartTrackHost;
  RowopsCommandDeps& rowopsCommandDeps;
  std::atomic<bool>& running;
  SamplerCommandDeps& samplerCommandDeps;
  const std::function<std::unique_ptr<TrackRuntime>(uint32_t, const std::string&, bool, bool)>& setupTrackRuntime;
  const std::function<SongStoreState()>& snapshotSongStore;
  const std::function<TrackStoreState(const TrackRuntime&)>& snapshotTrackStore;
  const std::function<std::vector<TrackRuntime*>()>& snapshotTracks;
  daw::TimeSignatureMap& songMeter;
  std::atomic<uint32_t>& songTimeSigDen;
  std::atomic<uint32_t>& songTimeSigNum;
  daw::TempoMapProvider& tempoProvider;
  TrackCommandDeps& trackCommandDeps;
  TrackpropsCommandDeps& trackpropsCommandDeps;
  std::vector<std::unique_ptr<TrackRuntime>>& tracks;
  std::mutex& tracksMutex;
  std::atomic<uint64_t>& transportElapsedNanotick;
  std::atomic<uint64_t>& transportNanotick;
  UndoCommandDeps& undoCommandDeps;
};

// Dispatches one UI command. Silently ignores entries that are not UiCommand or are too
// short for a header — the same two guards it opened with inside main().
void handleUiEntry(HandleUiEntryDeps& deps, const daw::EventEntry& entry);

}  // namespace daw::engine
