#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

#include "apps/event_payloads.h"
#include "apps/musical_structures.h"

namespace daw {

enum class UndoType : uint8_t {
  AddNote,
  RemoveNote,
};

struct UndoEntry {
  UndoType type = UndoType::AddNote;
  uint32_t trackId = 0;
  uint64_t nanotick = 0;
  uint64_t duration = 0;
  uint8_t pitch = 0;
  uint8_t velocity = 0;
};

struct ClipEditResult {
  UiDiffPayload diff{};
  uint32_t nextClipVersion = 0;
  std::optional<UndoEntry> undo;
};

bool requireMatchingClipVersion(uint32_t baseVersion,
                                uint32_t currentVersion,
                                UiDiffPayload& diffOut);

ClipEditResult addNoteToClip(MusicalClip& clip,
                             uint32_t trackId,
                             uint64_t nanotick,
                             uint64_t duration,
                             uint8_t pitch,
                             uint8_t velocity,
                             std::atomic<uint32_t>& clipVersion,
                             bool recordUndo);

std::optional<ClipEditResult> removeNoteFromClip(MusicalClip& clip,
                                                 uint32_t trackId,
                                                 uint64_t nanotick,
                                                 uint8_t pitch,
                                                 std::atomic<uint32_t>& clipVersion,
                                                 bool recordUndo);

}  // namespace daw
