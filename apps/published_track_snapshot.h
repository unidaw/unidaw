#pragma once

#include <atomic>
#include <memory>

// THE ONLY WAY TO PUBLISH OR READ A TRACK'S SNAPSHOT — enforced by the compiler, not by a scan.
//
// AE-P1.2 G2-B item 18, P-SNAPSHOT-PUBLISHERS: "Exactly twenty-four production TrackStateSnapshot
// publications exist: three prepublication assignments and twenty-one atomic stores; this packet
// does not silently treat them as a coherent host-plan authority."
//
// WHY THIS TYPE EXISTS, and it is not a style preference. The population above was inventoried by a
// script that scanned C++ for the shapes a publication can take. Two independent reviews defeated
// it, the second one ELEVEN WAYS after the first eight had been closed:
//
//     (*rt).trackSnapshot = snap;          a receiver the assignment pattern did not match
//     rt->trackSnapshot.swap(snap);        shared_ptr has swap; no `&`, no store, no `=`
//     publishInto(rt->trackSnapshot, ...)  a helper taking a reference rather than a pointer
//     SnapSlot& slot(rt) { return ...; }   a function returning the member; the call site never
//                                          names it
//     auto&& slot = rt->trackSnapshot;     one `&` more than the reference-bind pattern knew
//     ... and six more.
//
// Every fix widened a pattern and the next shape was outside the new one. That loop does not
// terminate, because "every way to write a std::shared_ptr member" is not a regular language. The
// surface has to go instead of being watched — the same conclusion this repository reached about
// the two loose-integer plugin-path helpers.
//
// WITH THIS TYPE, all eleven are COMPILE ERRORS: there is no assignment operator, no swap, no
// accessor that yields the address, and the slot is private. A publication can only be spelled
// `publish(...)` or `assignBeforePublication(...)`, so counting them is exact by construction and
// the inventory script's job shrinks to counting two named calls.
//
// THE TWO METHODS ARE THE RECORD'S TWO CATEGORIES, deliberately. Collapsing them into one would
// make the frozen "three prepublication assignments and twenty-one atomic stores" unverifiable —
// the split is part of what the record states, so it stays a distinction the code can be counted
// against.

namespace daw::engine {

struct TrackStateSnapshot;

class PublishedTrackSnapshot {
 public:
  PublishedTrackSnapshot() = default;

  // NOT COPYABLE OR ASSIGNABLE. `rt->trackSnapshot = snap` is the shape that started this, and it
  // is a compile error rather than an untracked publication.
  PublishedTrackSnapshot(const PublishedTrackSnapshot&) = delete;
  PublishedTrackSnapshot& operator=(const PublishedTrackSnapshot&) = delete;

  // ONE OF THE TWENTY-ONE ATOMIC STORES. Release, paired with the acquire in load(): a consumer
  // that sees this pointer sees everything written into the snapshot before it.
  void publish(std::shared_ptr<const TrackStateSnapshot> next) {
    std::atomic_store_explicit(&slot_, std::move(next), std::memory_order_release);
  }

  // ONE OF THE THREE PREPUBLICATION ASSIGNMENTS: the runtime is not yet reachable by any other
  // thread, so no atomic is needed and using one would suggest a race that cannot exist. It is a
  // DIFFERENT method rather than an overload precisely so the two populations stay countable
  // apart — and so a caller has to say which situation it is in.
  void assignBeforePublication(std::shared_ptr<const TrackStateSnapshot> initial) {
    slot_ = std::move(initial);
  }

  std::shared_ptr<const TrackStateSnapshot> load() const {
    return std::atomic_load_explicit(&slot_, std::memory_order_acquire);
  }

  explicit operator bool() const { return load() != nullptr; }

 private:
  // NO ACCESSOR RETURNS ITS ADDRESS, and none may be added: an address that escapes is a
  // publication path no count can follow, which is the whole reason this class exists.
  std::shared_ptr<const TrackStateSnapshot> slot_;
};

}  // namespace daw::engine
