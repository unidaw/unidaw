#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "apps/execution_snapshot.h"

// THE ONE PLACE A SNAPSHOT IS PUBLISHED, AND THE ONLY WAY TO PUBLISH ONE.
//
// AE-P1.2 G2-B item 18, R-HOST-PLAN-AUTHORITY:
//
//   "Under the command-thread writer lock, an authored mutation or undo/redo document is applied to
//    a candidate, the whole affected snapshot is compiled and globally validated, and one atomic
//    snapshot publication commits both; failure or revision exhaustion leaves the prior document,
//    high-water mark, and snapshot authoritative."
//
// COMPILE, VALIDATE AND PUBLISH ARE ONE CALL, and that is the entire reason this class exists. As
// three separate operations they can be performed in the wrong order, or two of them without the
// third — which is what twenty-four independent publications of overlapping state already are
// (P-SNAPSHOT-PUBLISHERS). A caller here cannot publish something it did not validate, because the
// only entry point does both.
//
// THE REVISION IS ALLOCATED BY THE STORE, not by the caller. A caller that chose its own revision
// could choose one that had already been used, and monotonicity would then depend on every caller
// getting it right. `publish` hands the candidate builder the revision it must stamp.
//
// WHAT A CONSUMER DOES. It takes a strong reference with `current()` and reads EVERYTHING from that
// one object. It never re-reads the store mid-block: two reads can straddle a publication, which is
// exactly the torn view this record removes. If it needs to know whether it is still current — a
// dispatcher that parked on a mutex, T-PLAN-RACE — it compares its own revision against
// `revision()` and refuses rather than proceeding.

namespace daw::engine {

class ExecutionSnapshotStore {
 public:
  // THE HIGHEST REVISION THIS STORE WILL PUBLISH, and it is a constructor parameter so that
  // exhaustion is TESTABLE rather than merely guarded.
  //
  // The default is the real ceiling. Reaching it through the public API would take 2^64
  // publications, so a test cannot get there — and the first version of this class was therefore
  // shipped with a test that asserted the exhaustion enum EXISTS and called that coverage. That is
  // a tautology wearing a test's name, and this project has shipped several.
  //
  // A ceiling is a genuine property of a store rather than a hole opened for a test: a caller that
  // wants to prove its own behaviour at exhaustion constructs one with a low ceiling and drives it
  // there. No test-only setter, no way to seed an unvalidated snapshot.
  explicit ExecutionSnapshotStore(uint64_t maxRevision = UINT64_MAX - 1)
      : maxRevision_(maxRevision) {}

  // The published revision, or nullptr before the first successful publish. Returned by value so a
  // consumer holds the object alive for as long as it reads from it — a raw pointer here would let
  // the next publication free the snapshot mid-read.
  std::shared_ptr<const daw::ExecutionSnapshot> current() const {
    return std::atomic_load_explicit(&published_, std::memory_order_acquire);
  }

  // The published revision, or 0 when nothing has been published. 0 is not a legal revision
  // (validateExecutionSnapshot refuses it), which is what lets "nothing yet" be a distinguishable
  // answer rather than a coincidence.
  //
  // THE PUBLISHED REVISION, READABLE WITHOUT TOUCHING THE SNAPSHOT.
  //
  // This is a second copy of `published_->revision`, which an earlier version of this class removed
  // as exactly the "one fact, two sources" defect this effort keeps finding. It is back, and the
  // difference is that the ordering and the failure direction are now SPECIFIED rather than left to
  // whatever the writes happened to do:
  //
  //   the revision is stored BEFORE the pointer, so a reader can see the new number with the old
  //   object but never the old number with the new one;
  //   therefore `isCurrent(held)` can only ever be WRONG in the direction of refusing — it may say
  //   "stale" a few nanoseconds early, and can never say "current" about a snapshot that has been
  //   replaced.
  //
  // A conservative answer is the one a dispatcher needs; the unsafe direction is the one that
  // matters, and it is unreachable. The earlier removal was right about the hazard and wrong that
  // the only fix was deletion.
  //
  // WHY IT IS WORTH THE DUPLICATION: on this toolchain `std::atomic_load(shared_ptr*)` is not
  // lock-free — it takes a process-wide mutex keyed on the address and copies the control block, and
  // dropping the copy can DEALLOCATE the whole snapshot on the caller's thread. This class is
  // written for "an offline or realtime dispatcher" (T-PLAN-RACE); asking one to take a lock and
  // risk a deallocation to read a uint64_t is not something a realtime path can do.
  uint64_t revision() const {
    return revision_.load(std::memory_order_acquire);
  }

  // AUTHOR, BUILD, PUBLISH. Returns false with `error` set and NOTHING CHANGED on any failure.
  //
  // TAKES AUTHORED DATA, NOT A SNAPSHOT, and that is what makes two failure modes unrepresentable
  // rather than checked:
  //
  //   A caller cannot hand in a snapshot it built elsewhere, because `buildExecutionSnapshot` is the
  //   only thing that makes one and this is the only thing that calls it inside the lock.
  //
  //   A caller cannot choose its own revision. The previous shape took a callback that was HANDED a
  //   revision and returned a snapshot, so it could return one stamped with a different number —
  //   a mistake that needed its own check, its own error code and its own test. The store stamps it
  //   now; there is nothing to disagree with.
  //
  // The writer lock is held across build, validate and store. That is the point: a second publisher
  // cannot interleave between the validation and the publication, so what gets published is always
  // what was validated.
  bool publish(std::vector<daw::AuthoredTrackPlan> tracks, uint32_t nextDeviceId,
               daw::PatcherGraph patcherGraph,
               const std::vector<uint32_t>& registeredInputIds,
               daw::SnapshotError* error) {
    std::lock_guard<std::mutex> lock(writerMutex_);

    auto previous = std::atomic_load_explicit(&published_, std::memory_order_acquire);
    const uint64_t previousRevision = previous ? previous->revision : 0;
    // REVISION EXHAUSTION IS CHECKED BEFORE ANYTHING IS BUILT. "failure or revision exhaustion
    // leaves the prior document, high-water mark, and snapshot authoritative" — so it must not be
    // discovered after a build has already happened.
    if (previousRevision >= maxRevision_) {
      if (error != nullptr) {
        *error = daw::SnapshotError{};
        error->code = daw::SnapshotErrorCode::RevisionExhausted;
      }
      return false;
    }

    auto candidate = daw::buildExecutionSnapshot(previousRevision + 1, nextDeviceId,
                                                 std::move(tracks), std::move(patcherGraph),
                                                 registeredInputIds, previous.get(), error);
    if (!candidate) {
      return false;
    }

    // THE REVISION FIRST, THEN THE POINTER, and the order is the whole of the guarantee above. A
    // reader between the two stores sees the NEW number and the OLD object, so `isCurrent` refuses;
    // the reverse order would let it approve a holder whose snapshot had already been replaced.
    auto sealed = std::make_shared<const daw::ExecutionSnapshot>(std::move(*candidate));
    revision_.store(sealed->revision, std::memory_order_release);
    std::atomic_store_explicit(&published_, sealed, std::memory_order_release);
    return true;
  }

  // WHETHER A HELD REVISION IS STILL THE PUBLISHED ONE. T-PLAN-RACE: "An offline or realtime
  // dispatcher that loaded ExecutionSnapshot N and parks on controllerMutex refuses after snapshot
  // N+1 commits."
  //
  // A dispatcher asks this AFTER acquiring the lock it parked on, not before — the whole hazard is
  // the window between deciding to act and being able to.
  bool isCurrent(uint64_t heldRevision) const {
    return heldRevision != 0 && heldRevision == revision();
  }

 private:
  mutable std::mutex writerMutex_;
  const uint64_t maxRevision_;
  // Written BEFORE published_, read without a lock. See revision().
  std::atomic<uint64_t> revision_{0};
  std::shared_ptr<const daw::ExecutionSnapshot> published_;
};

}  // namespace daw::engine
