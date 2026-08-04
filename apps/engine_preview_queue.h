#pragma once
// AUDITIONED NOTES ON THEIR WAY TO THE PRODUCER — a queue, its lock, and what is still sounding.
//
// The UI thread asks for a preview; the producer — the sole writer of the per-track event rings —
// drains the queue once per block and emits the note. The three pieces have never been apart: the
// mutex guards both the queue and the held-pitch map, and every reader of one reads the other
// under the same lock. They were three separate main() locals passed individually into three
// different Deps structs, which is seven member slots for one idea.
//
// heldPreview IS NOT BOOKKEEPING. It tracks the pitches currently sounding per track, so Stop (and
// a track being removed, and a panic) can send the note-offs that were never sent. Without it an
// audition that is interrupted rather than released hangs forever — the classic stuck note.
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "engine_types.h"

namespace daw::engine {

class PreviewQueue {
 public:
  // PUBLIC because the producer and the transport commands read them directly and always have.
  // Making them private would mean an accessor per member and a diff that is no longer verbatim;
  // the point is that the state stopped being scattered, not that it grew an interface.
  std::mutex previewMutex;
  std::vector<PreviewNoteReq> pendingPreviewNotes;
  std::unordered_map<uint32_t, std::vector<uint8_t>> heldPreview;  // trackId -> held pitches

  // Enqueue an audition and update the held-pitch set. Caller holds nothing; this locks.
  void enqueuePreview(uint32_t trackId, uint8_t pitch, uint8_t velocity, bool on);
};

}  // namespace daw::engine
