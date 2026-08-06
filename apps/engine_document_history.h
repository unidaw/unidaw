#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "apps/project_file.h"

namespace daw::engine {

// UNDO AS A CURSOR OVER DOCUMENT VERSIONS.
//
// Owner's ruling: every operation must be undoable, and redo must be the exact opposite of undo.
// This is the structure that makes both true by construction rather than by discipline.
//
// The old model was a stack of EngineUndoEntry, each carrying a hand-picked subset of a track's
// state — TrackStoreState is {placements, clips, editable}, three fields — so 55 of 70 mutating
// commands had nothing to record and no way to record it. The subset was the defect: a command
// could not be undoable unless somebody had thought to widen the struct for it.
//
// A version is the WHOLE authored document. There is no subset to get wrong, and a field added to
// ProjectTrack next year is undoable the day it is added, because undo does not enumerate fields.
//
// REDO IS NOT A SECOND MECHANISM. It is `cursor_ + 1` where undo is `cursor_ - 1`, and both then
// apply versions_[cursor_]. There is no invert() to be a not-quite-inverse of anything — the
// engine previously had one, hand-written, used by BOTH handleUndo and handleRedo, with three
// branches each that had to stay consistent. A state you return to is bit-identical to the state
// you left because it is literally the same object.
//
// A BYTE BUDGET, NOT A STEP COUNT. One entry can be a 9 MB ripple on a 100k-note song and the
// next a 120-byte rename; a fixed number of steps therefore bounds nothing. Reaper ships this as
// a user-visible "undo memory (MB)" preference for the same reason. Oldest versions are evicted
// first, and version 0 is never evicted while anything above it survives — dropping the base
// would make the remaining history unreachable rather than merely shorter.
//
// COST, STATED PLAINLY: a version today is a deep copy, so a 100k-note project pays ~9 MB per
// edit. That is the cost of correctness arriving before representation, which is the deliberate
// ordering — Step 3 makes the heavy leaves shared (shared_ptr<const T>) and the same history
// becomes ~100 bytes per edit with no change to this interface or to undo's behaviour. The
// expensive part is ALREADY paid today by the ten commands that work: the note vector dominates,
// so completeness is nearly free and the memory problem predates this structure.
class DocumentHistory {
 public:
  // ~256 MB by default: enough for a long session on an ordinary song, bounded on a huge one.
  explicit DocumentHistory(size_t byteBudget = 256ull * 1024 * 1024)
      : byteBudget_(byteBudget) {}

  // The base version, recorded once when the engine first has a document worth returning to.
  // Without it the first undo has no earlier state to reach and would silently do nothing.
  void seed(daw::ProjectDocument doc) {
    std::lock_guard<std::mutex> lock(mutex_);
    versions_.clear();
    labels_.clear();
    versions_.push_back(std::move(doc));
    labels_.emplace_back("open");
    cursor_ = 0;
    bytes_ = approximateBytes(versions_.front());
  }

  // Record the state AFTER an edit. `label` is required, not optional: a version nobody can name
  // is a version nobody can present in a menu, and making it a parameter means a new command
  // cannot be recorded anonymously by accident.
  void commit(daw::ProjectDocument doc, std::string label) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (versions_.empty()) {
      versions_.push_back(doc);
      labels_.emplace_back("open");
      cursor_ = 0;
      bytes_ = approximateBytes(versions_.front());
    }
    // COMMITTING AFTER AN UNDO DISCARDS THE REDO TAIL, which is what every editor does and what
    // the old stack did explicitly. Dropping it here rather than at the call site means it cannot
    // be forgotten by a future command.
    while (versions_.size() > cursor_ + 1) {
      bytes_ -= approximateBytes(versions_.back());
      versions_.pop_back();
      labels_.pop_back();
    }
    bytes_ += approximateBytes(doc);
    versions_.push_back(std::move(doc));
    labels_.push_back(std::move(label));
    cursor_ = versions_.size() - 1;
    evictOldest();
  }

  // Undo and redo are the same motion with the sign flipped. Both return the version to apply,
  // or nullptr when there is nowhere to go — the caller does not need to know which end it is at.
  const daw::ProjectDocument* undo() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cursor_ == 0 || versions_.empty()) {
      return nullptr;
    }
    --cursor_;
    return &versions_[cursor_];
  }

  const daw::ProjectDocument* redo() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (versions_.empty() || cursor_ + 1 >= versions_.size()) {
      return nullptr;
    }
    ++cursor_;
    return &versions_[cursor_];
  }

  // What the menu would say. Empty when there is nothing to undo/redo.
  std::string undoLabel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (cursor_ == 0 || labels_.empty()) ? std::string() : labels_[cursor_];
  }
  std::string redoLabel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (cursor_ + 1 < labels_.size()) ? labels_[cursor_ + 1] : std::string();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return versions_.size();
  }
  size_t cursor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cursor_;
  }
  size_t bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
  }

 private:
  // APPROXIMATE ON PURPOSE, and only the parts that dominate. The note vectors are three orders
  // of magnitude larger than everything else (MusicalEvent is 112 B and a stress project holds
  // 80,896 of them), so counting them plus a flat per-track allowance is within noise of an exact
  // figure and costs no traversal of the small stuff. A budget does not need precision, it needs
  // to be monotonic in the thing that actually grows.
  static size_t approximateBytes(const daw::ProjectDocument& doc) {
    size_t total = sizeof(daw::ProjectDocument);
    for (const auto& clip : doc.clips) {
      total += sizeof(daw::ProjectClip) +
               clip.clip.events().size() * sizeof(daw::MusicalEvent);
    }
    for (const auto& track : doc.tracks) {
      total += sizeof(daw::ProjectTrack) + 4096;  // chain, sampler banks, graphs, automation
      for (const auto& pl : track.placements) {
        total += sizeof(daw::ProjectPlacement) + pl.adds.size() * sizeof(daw::MusicalEvent);
      }
    }
    return total;
  }

  void evictOldest() {
    // Never evict below two versions: with one there is nothing to undo TO, and the budget is
    // meant to bound a long session, not to defeat undo on a project that is simply large.
    while (bytes_ > byteBudget_ && versions_.size() > 2 && cursor_ > 0) {
      bytes_ -= approximateBytes(versions_.front());
      versions_.erase(versions_.begin());
      labels_.erase(labels_.begin());
      --cursor_;
    }
  }

  mutable std::mutex mutex_;
  std::vector<daw::ProjectDocument> versions_;
  std::vector<std::string> labels_;
  size_t cursor_ = 0;
  size_t bytes_ = 0;
  size_t byteBudget_;
};

}  // namespace daw::engine
