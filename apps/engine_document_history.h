#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "apps/document_compare.h"
#include "apps/engine_plugin_state_version.h"
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
  void seed(daw::ProjectDocument doc, PluginStateSnapshot plugins = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    versions_.clear();
    labels_.clear();
    pluginStates_.clear();
    versions_.push_back(std::move(doc));
    labels_.emplace_back("open");
    pluginStates_.push_back(std::move(plugins));
    cursor_ = 0;
    bytes_ = approximateBytes(versions_.front());
  }

  // Record the state AFTER an edit. `label` is required, not optional: a version nobody can name
  // is a version nobody can present in a menu, and making it a parameter means a new command
  // cannot be recorded anonymously by accident.
  //
  // RETURNS whether a version was actually recorded. `false` means the document was byte-identical
  // to the one already at the cursor — see below.
  bool commit(daw::ProjectDocument doc, std::string label,
              PluginStateSnapshot plugins = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (versions_.empty()) {
      versions_.push_back(doc);
      labels_.emplace_back("open");
      pluginStates_.push_back(plugins);
      cursor_ = 0;
      bytes_ = approximateBytes(versions_.front());
    }

    // A COMMAND THAT CHANGED NOTHING DOES NOT OPEN AN UNDO STEP — and, far more importantly,
    // DOES NOT DESTROY THE REDO TAIL.
    //
    // The recording bracket fires on every mutating opcode, whether or not the handler agreed to
    // do anything. Handlers refuse constantly and by design: a stale baseVersion, an unknown
    // track, an out-of-range slot. So the everyday sequence "undo, then send a command that gets
    // refused" truncated everything ahead of the cursor and left the user with no redo and no
    // change to show for it. That is data loss caused by a command the engine explicitly declined.
    //
    // THE TEST IS THE DOCUMENT, NOT THE HANDLER'S OPINION. An outcome flag threaded through 48
    // branches would be 48 chances to forget one, and it would still miss the handler that
    // succeeds while writing the value that was already there. Comparing what SAVE WOULD WRITE
    // is the same definition undo itself uses, so the two cannot disagree.
    //
    // STAGE 3 LANDED HERE. This used to serialize the whole document to JSON and compare strings
    // against a cached copy — correct, and megabytes of text per mutating command on a large
    // project. documentFieldsEqual walks the field lists and stops at the first difference.
    //
    // THE SWAP WAS NOT MADE ON THE STRENGTH OF THE COMPARER LOOKING RIGHT. A field the walk cannot
    // see makes this answer "nothing changed" for an edit that changed it: no version, no undo
    // step, silent and permanent, and indistinguishable from a refused command.
    // comparer_equivalence_tests perturbs every leaf a save writes, on every shipped preset, and
    // requires the comparer to notice. Its first run found 29 blind fields — all of Device's
    // patcher graph, euclidean config, vst_ref and sampler — and a live persistence bug on the way
    // (load_mode written as a boolean, so a by-path plugin reloaded by-reference). The switch was
    // made when that check went green with an EMPTY baseline, and it stays true because the check
    // is in the gate.
    // SHARE THE BYTES OF ANY PLUGIN THAT DID NOT CHANGE, before the comparison below — which then
    // costs a pointer compare per device rather than a memcmp of every blob in the project.
    if (cursor_ < pluginStates_.size()) {
      sharePluginBlobsWith(pluginStates_[cursor_], plugins);
    }

    // "NOTHING CHANGED" MUST INCLUDE THE PLUGINS, or the single most common plugin edit is
    // un-undoable. A VST's state is not in ProjectDocument, so turning a cutoff leaves the
    // serialized document byte-identical; testing the document alone would classify it as a
    // refused command, record no version, and Ctrl-Z would step over the knob turn as if it had
    // never happened. The test is "did anything a version holds change", and a version holds both.
    const bool documentSame =
        cursor_ < versions_.size() && daw::documentFieldsEqual(versions_[cursor_], doc);
    const bool pluginsSame =
        cursor_ < pluginStates_.size() && samePluginState(pluginStates_[cursor_], plugins);
    if (documentSame && pluginsSame) {
      return false;
    }
    // COMMITTING AFTER AN UNDO DISCARDS THE REDO TAIL, which is what every editor does and what
    // the old stack did explicitly. Dropping it here rather than at the call site means it cannot
    // be forgotten by a future command.
    while (versions_.size() > cursor_ + 1) {
      bytes_ -= approximateBytes(versions_.back());
      versions_.pop_back();
      labels_.pop_back();
      pluginStates_.pop_back();
    }
    bytes_ += approximateBytes(doc);
    versions_.push_back(std::move(doc));
    labels_.push_back(std::move(label));
    pluginStates_.push_back(std::move(plugins));
    cursor_ = versions_.size() - 1;
    // THE OPEN GESTURE NOW OWNS A VERSION, so everything after this may amend it. Until this line
    // runs there is nothing of the gesture's own to rewrite — see gestureAmendable().
    if (gestureOpen_) {
      gestureHasVersion_ = true;
    }
    evictOldest();
    return true;
  }

  // REWRITE THE VERSION AT THE CURSOR instead of pushing a new one.
  //
  // For a command that changes the document but must not open an undo step — today only the A/B
  // audition swap. Skipping the recording entirely would be wrong in a way that is hard to see:
  // the version at the cursor would still hold the PREVIOUS audition state, so the next undo would
  // restore it and silently flip the placement back as collateral on an unrelated edit. Amending
  // keeps history and the live document in agreement while leaving the step count alone.
  //
  // THE REDO TAIL SURVIVES. An audition is not an edit, so it has no business destroying work the
  // user can still redo — which is the same reason it does not push a version.
  void amend(daw::ProjectDocument doc, PluginStateSnapshot plugins = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (versions_.empty()) {
      return;
    }
    bytes_ -= approximateBytes(versions_[cursor_]);
    bytes_ += approximateBytes(doc);
    versions_[cursor_] = std::move(doc);
    if (cursor_ < pluginStates_.size()) {
      sharePluginBlobsWith(pluginStates_[cursor_], plugins);
      pluginStates_[cursor_] = std::move(plugins);
    }
  }

  // REWRITE ONLY THE PLUGIN STATE AT THE CURSOR, leaving the document alone.
  //
  // For the interrupted drag. A gesture defers its plugin capture to the command carrying END —
  // one cross-process round trip per drag instead of one per sample, which is the whole reason
  // coalescing had to land before this. When the pointer never comes back up there is no END, and
  // the drag's version would keep the plugin state from BEFORE the drag: undoing to that step
  // would restore the document the drag produced and the plugin as it was beforehand, which is a
  // state that never existed. The force-close path calls this so the abandoned step still holds
  // the plugin state that goes with its document.
  void amendPluginState(PluginStateSnapshot plugins) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cursor_ >= pluginStates_.size()) {
      return;
    }
    sharePluginBlobsWith(pluginStates_[cursor_], plugins);
    pluginStates_[cursor_] = std::move(plugins);
  }

  // The plugin state belonging to the version at the cursor — what undo/redo must push after
  // applying the document. Empty when nothing was ever captured, which a restore reads as
  // "nothing to push" rather than "push nothing", and those differ: see restorePluginSnapshot.
  PluginStateSnapshot pluginStateAtCursor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cursor_ < pluginStates_.size() ? pluginStates_[cursor_] : PluginStateSnapshot{};
  }

  // A GESTURE IS ONE UNDO STEP. See kUiCmdFlagGestureBegin in event_payloads.h for why the UI has
  // to tell us rather than the engine guessing.
  //
  // beginGesture() only marks the state; the command carrying BEGIN still commits normally, so the
  // step exists from the first movement and an interrupted drag is still undoable. Everything
  // after it amends until endGesture().
  // `openedBy` is the command type that started the drag. The force-close guard needs it to tell
  // the drag's OWN middle from an unrelated later edit — see gestureForceCloseFor().
  void beginGesture(uint32_t openedBy = 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    gestureOpen_ = true;
    gestureHasVersion_ = false;
    gestureOpenedBy_ = openedBy;
  }
  void endGesture() {
    std::lock_guard<std::mutex> lock(mutex_);
    gestureOpen_ = false;
    gestureHasVersion_ = false;
    gestureOpenedBy_ = 0;
  }

  // SHOULD THIS COMMAND FORCE AN OPEN GESTURE SHUT?
  //
  // THE BUG THIS REPLACES, measured by frontend on a real drag: the guard closed on ANY mutating
  // command carrying neither flag. A drag's own middle moves carry neither — only its first and
  // last do — so the first pointermove after BEGIN closed the gesture BEGIN had just opened. Eight
  // sends produced eight versions. The coalescing worked and was unreachable by any UI following
  // the documented contract, and beginGesture's own comment ("everything after it amends until
  // endGesture") was false.
  //
  // It went unnoticed because gesture_undo_tests drives DocumentHistory DIRECTLY and never runs
  // the bracket. That file says so in its own header — the semantics were pinned, the wiring was
  // not — and this is the defect that gap was hiding. tools/gesture_drag_check.sh now drives real
  // commands with the real flags.
  //
  // THE RULE: a command of the SAME TYPE as the one that opened the gesture is the drag continuing.
  // Anything else is a different edit and closes it. That is a proxy for intent rather than a
  // reading of it, and its residual exposure is stated rather than hidden: if a UI dies mid-drag
  // and the user then edits ANOTHER parameter, that edit is of the same type and joins the dead
  // drag's step instead of opening its own. Undo granularity degrades until any other command type
  // arrives; nothing is lost, because gestureAmendable() still protects the pre-drag version.
  // Closing that last gap needs the UI to say "I am gone", which no wire signal carries today.
  bool gestureForceCloseFor(uint32_t commandType) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gestureOpen_ && commandType != gestureOpenedBy_;
  }
  bool gestureOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gestureOpen_;
  }

  // MAY THE NEXT COMMAND AMEND? Only once the gesture has pushed a version of ITS OWN.
  //
  // "Gesture open" is not sufficient, and assuming it was destroyed the pre-drag state. commit()
  // returns false for a document byte-identical to the cursor's, so a drag whose first command
  // changes nothing — a pointer-down before any travel, or a SetDeviceParam while params still
  // live outside the document — opens the gesture with the cursor still on the PRE-DRAG version.
  // Every amend() after that overwrites the state the user wants Ctrl-Z to return them to.
  //
  // With this, a gesture that has not yet committed keeps committing until one command actually
  // changes something; that command's version becomes the step, and the rest of the drag rewrites
  // it. One drag is still one step, and the step is never the one before the drag.
  bool gestureAmendable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gestureOpen_ && gestureHasVersion_;
  }

  // Undo and redo are the same motion with the sign flipped. Both return the version to apply,
  // or nullptr when there is nowhere to go — the caller does not need to know which end it is at.
  const daw::ProjectDocument* undo() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cursor_ == 0 || versions_.empty()) {
      return nullptr;
    }
    --cursor_;
    // NO CACHE TO KEEP IN STEP any more, and that is the second thing stage 3 bought. The string
    // cache had to be refreshed on every path that moved cursor_, and a stale one did not merely
    // mislead a log line — it decided whether an edit was recorded at all. The comparer reads
    // versions_[cursor_] directly, so "compare against where the cursor is" is true by
    // construction rather than by four call sites remembering.
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
      if (!pluginStates_.empty()) {
        pluginStates_.erase(pluginStates_.begin());
      }
      --cursor_;
    }
  }

  mutable std::mutex mutex_;
  // OPEN MEANS "AMEND, DO NOT PUSH". Force-closed by the command bracket when a non-gesture
  // command arrives, so a UI that dies mid-drag cannot wedge every later edit into one step.
  bool gestureOpen_ = false;
  // Whether the OPEN gesture has committed a version of its own yet. See gestureAmendable().
  bool gestureHasVersion_ = false;
  // The command type that opened the gesture, so its own repeats are not mistaken for a new edit.
  uint32_t gestureOpenedBy_ = 0;
  std::vector<daw::ProjectDocument> versions_;
  std::vector<std::string> labels_;
  // PARALLEL TO versions_, one entry per version. Kept as a separate vector rather than a field on
  // ProjectDocument because a plugin blob is not part of the authored song: it must not be
  // serialized into the project file, compared by the document comparer, or reach any code that
  // treats a ProjectDocument as the thing a user wrote. Every push, pop and erase above touches
  // all three vectors together — the pairing is the invariant.
  std::vector<PluginStateSnapshot> pluginStates_;
  size_t cursor_ = 0;
  size_t bytes_ = 0;
  size_t byteBudget_;
};

}  // namespace daw::engine
