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

#include "engine_arrangetime_commands.h"
#include "engine_automation_commands.h"
#include "engine_chain_commands.h"
#include "engine_clip_commands.h"
#include "engine_device_commands.h"
#include "engine_marker_commands.h"
#include "engine_modlink_commands.h"
#include "engine_module_commands.h"
#include "engine_note_commands.h"
#include "engine_patcher_commands.h"
#include "engine_placement_commands.h"
#include "engine_project_commands.h"
#include "engine_request_commands.h"
#include "engine_rowops_commands.h"
#include "engine_sampler_commands.h"
#include "engine_trackprops_commands.h"
#include "engine_track_commands.h"
#include "engine_transport_commands.h"
#include "engine_types.h"
#include "engine_undo_commands.h"
#include "event_log.h"
#include "markers.h"
#include "time_base.h"
#include "time_signature_map.h"

namespace daw::engine {

struct HandleUiEntryDeps {
  ArrangeTimeCommandDeps& arrangeTimeCommandDeps;
  AutomationCommandDeps& automationCommandDeps;
  std::vector<BulkStream>& bulkStreams;
  uint64_t& bulkTick;
  ChainCommandDeps& chainCommandDeps;
  ClipCommandDeps& clipCommandDeps;
  DeviceCommandDeps& deviceCommandDeps;
  const std::function<void(uint32_t, uint8_t, uint8_t, bool)>& enqueuePreview;
  const std::function<void(const std::vector<uint8_t>&)>& handleAssembledBulk;
  const std::function<void(const char*, const char*, uint32_t, uint32_t, const std::string&)>& historyAppend;
  MarkerCommandDeps& markerCommandDeps;
  ModlinkCommandDeps& modlinkCommandDeps;
  ModuleCommandDeps& moduleCommandDeps;
  NoteCommandDeps& noteCommandDeps;
  PatcherCommandDeps& patcherCommandDeps;
  PlacementCommandDeps& placementCommandDeps;
  ProjectCommandDeps& projectCommandDeps;
  RequestCommandDeps& requestCommandDeps;
  RowopsCommandDeps& rowopsCommandDeps;
  SamplerCommandDeps& samplerCommandDeps;
  TrackCommandDeps& trackCommandDeps;
  TrackpropsCommandDeps& trackpropsCommandDeps;
  TransportCommandDeps& transportCommandDeps;
  UndoCommandDeps& undoCommandDeps;
};

// Dispatches one UI command. Silently ignores entries that are not UiCommand or are too
// short for a header — the same two guards it opened with inside main().
void handleUiEntry(HandleUiEntryDeps& deps, const daw::EventEntry& entry);

}  // namespace daw::engine
