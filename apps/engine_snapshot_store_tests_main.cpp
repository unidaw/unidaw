// THE PUBLICATION TRANSACTION: author, build, publish — or change nothing.
//
// AE-P1.2 G2-B item 18, R-HOST-PLAN-AUTHORITY: "Under the command-thread writer lock, an authored
// mutation or undo/redo document is applied to a candidate, the whole affected snapshot is compiled
// and globally validated, and one atomic snapshot publication commits both; failure or revision
// exhaustion leaves the prior document, high-water mark, and snapshot authoritative."
//
// And T-PLAN-RACE: "An offline or realtime dispatcher that loaded ExecutionSnapshot N and parks on
// controllerMutex refuses after snapshot N+1 commits; authored mutation or undo/redo WITHOUT
// PUBLICATION leaves every execution consumer on N."
//
// The second half of that sentence is the easy one to miss. A test that only proves a stale holder
// refuses would pass an implementation that refused everything; the other half — a mutation that
// does not publish leaves every consumer exactly where it was — is what makes the refusal mean
// something.
//
// ONE TEST IS GONE AND NOTHING REPLACED IT. `aBuilderMayNotChooseItsOwnRevision` checked that a
// candidate stamped with a revision it was not handed is refused. `publish` now takes AUTHORED data
// and stamps the revision itself, so there is nothing to disagree with — the failure mode, its error
// code and its test all disappeared together.

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "apps/engine_snapshot_store.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::printf("engine_snapshot_store_tests: FAIL %s\n", message.c_str());
    ++failures;
  }
}

std::vector<daw::AuthoredTrackPlan> authored(const std::string& pluginName = "eq") {
  daw::DevicePlan device;
  device.stableDeviceId = 7;
  device.kind = daw::DeviceKind::VstEffect;
  device.occupancy = daw::SlotOccupancy::Occupies;
  device.compactIndex = 0;
  device.resolvedPluginPath = "/plugins/" + pluginName + ".vst3";
  device.resolvedPluginName = pluginName;

  daw::AuthoredTrackPlan plan;
  plan.trackId = 0;
  plan.devices.push_back(device);

  // AND A MASTER, because every session has one and a snapshot without one is now refused. Omitting
  // it made `publish` fail, `current()` stay null, and the next dereference SEGV — which is how this
  // was found. The tests are about the STORE, so the session they publish has to be a legal one.
  daw::AuthoredTrackPlan master;
  master.trackId = 900;
  master.isMaster = true;
  return {plan, master};
}

bool publish(daw::engine::ExecutionSnapshotStore& store, daw::SnapshotError* error,
             const std::string& pluginName = "eq", uint32_t nextDeviceId = 8) {
  return store.publish(authored(pluginName), nextDeviceId, daw::PatcherGraph{}, {}, error);
}

std::string pluginOf(const daw::ExecutionSnapshot& s) {
  // BY IDENTITY, not by position. `tracks.front()` was the authored track only because the master
  // happened to be appended second; a helper that depends on the order of a vector nobody promised
  // is a test that breaks for a reason unrelated to what it asserts.
  for (const auto& plan : s.tracks) {
    if (!plan.devices.empty()) {
      return plan.devices.front().resolvedPluginName;
    }
  }
  return "";
}

void nothingIsPublishedUntilSomethingIs() {
  daw::engine::ExecutionSnapshotStore store;
  expect(store.current() == nullptr, "a fresh store publishes nothing");
  expect(store.revision() == 0,
         "and reports revision 0 — not a legal revision, so 'nothing yet' is a value rather than a "
         "coincidence");
  expect(!store.isCurrent(0), "revision 0 is never current");
  expect(!store.isCurrent(1), "and neither is a revision nobody published");
}

void publishingStampsTheRevisionTheStoreChose() {
  daw::engine::ExecutionSnapshotStore store;
  daw::SnapshotError e;
  expect(publish(store, &e), "the first publish succeeds");
  expect(store.revision() == 1, "the store hands out revision 1 first — nonzero, as required");
  expect(store.current() != nullptr && store.current()->revision == 1,
         "and the published object carries that same revision; there is only one copy of it");
  expect(store.isCurrent(1), "revision 1 is current");

  expect(publish(store, &e, "comp"), "the second publish succeeds");
  expect(store.revision() == 2, "and takes revision 2");
  expect(!store.isCurrent(1), "revision 1 is no longer current");
  expect(store.isCurrent(2), "revision 2 is");
}

void aFailedBuildChangesNothing() {
  daw::engine::ExecutionSnapshotStore store;
  daw::SnapshotError e;
  publish(store, &e);
  auto before = store.current();

  // "failure ... leaves the prior document, high-water mark, and snapshot authoritative."
  auto bad = authored("comp");
  bad.push_back(bad.front());  // two plans for one track id
  expect(!store.publish(bad, 8, daw::PatcherGraph{}, {}, &e),
         "an invalid authored session must not publish");
  expect(e.code == daw::SnapshotErrorCode::DuplicateTrackId, "...naming the rule it broke");
  expect(store.current() == before,
         "and the SAME object is still published — not an equal one, the same one");
  expect(store.revision() == 1, "on the same revision");
  expect(pluginOf(*store.current()) == "eq", "carrying the plan it had before");

  expect(publish(store, &e, "verb"), "a valid session publishes after a rejected one");
  expect(store.revision() == 2, "and takes the next revision — the failed attempt consumed none");
}

void aStaleHolderRefusesAndACurrentOneDoesNot() {
  daw::engine::ExecutionSnapshotStore store;
  daw::SnapshotError e;
  publish(store, &e);

  auto held = store.current();
  const uint64_t heldRevision = held->revision;
  expect(store.isCurrent(heldRevision), "while nothing else publishes, the holder is current");

  // "authored mutation or undo/redo without publication leaves every execution consumer on N" — a
  // failed publish is exactly that, and it must NOT invalidate a holder.
  auto bad = authored();
  bad.front().devices.front().resolvedPluginPath.clear();
  store.publish(bad, 8, daw::PatcherGraph{}, {}, &e);
  expect(store.isCurrent(heldRevision),
         "a mutation that did not publish leaves the holder current — a refusal must not invalidate "
         "a consumer that nothing replaced");

  publish(store, &e, "comp");
  expect(!store.isCurrent(heldRevision), "once N+1 commits, the holder of N is stale and refuses");
  expect(pluginOf(*held) == "eq",
         "and its own copy is UNCHANGED — it holds a strong reference, so the publication did not "
         "move the ground under it");
  // `isCurrent(store.revision())` would be `r != 0 && r == r` — a tautology that can only fail if
  // nothing was ever published. The revision is named instead, so the assertion says which one.
  expect(store.revision() == heldRevision + 1, "the store advanced by exactly one revision");
  expect(store.isCurrent(heldRevision + 1), "and THAT revision is the current one");
}

void revisionExhaustionRefusesBeforeBuildingAnything() {
  // DRIVEN TO A REAL CEILING. An earlier version asserted that the exhaustion enum existed and
  // stopped there — a tautology that passed for any implementation, including one with no guard.
  // The store takes its ceiling as a constructor parameter precisely so this can be exercised.
  daw::engine::ExecutionSnapshotStore store(/*maxRevision=*/2);
  daw::SnapshotError e;
  expect(publish(store, &e, "p1"), "revision 1 publishes");
  expect(publish(store, &e, "p2"), "revision 2 publishes, reaching the ceiling");
  expect(store.revision() == 2, "the store is at its ceiling");

  expect(!publish(store, &e, "p3"), "the next publish must be refused");
  expect(e.code == daw::SnapshotErrorCode::RevisionExhausted, "...as exhaustion");
  expect(store.revision() == 2 && pluginOf(*store.current()) == "p2",
         "and the prior snapshot is still the authority, with its own plan intact");
  expect(!publish(store, &e, "p4"), "a second attempt is refused too");
  expect(store.revision() == 2, "and the store has not moved");
}

void manyPublishersAndManyReadersNeverSeeATornSnapshot() {
  // TWO CORRECTIONS LIVE IN THIS TEST, and the second was found by running it.
  //
  // FIRST: it had ONE writer — the main thread — under a comment claiming "two publishers cannot
  // interleave". With one writer the mutex is provably uncontended, and a reviewer deleted
  // `std::lock_guard<std::mutex> lock(writerMutex_)` outright with the suite still green.
  //
  // SECOND: the tearing detector was that the published plan's plugin name should equal
  // "p<revision>". That is not an invariant of the STORE — the store stamps `previousRevision + 1`
  // itself, so a caller that read the revision and then published cannot know which number its plan
  // will be given, and with four publishers it routinely guesses wrong. The test failed with the
  // lock in place, for its own reason.
  //
  // What IS intrinsic to every correctly built snapshot: `deviceOwner()` is derived from `tracks`,
  // so it has exactly one entry per device and every device's id is a key. A reader that ever
  // observed a half-published object would see those disagree. That holds no matter which publisher
  // produced the object, which is what makes it a property of the store rather than of the caller.
  daw::engine::ExecutionSnapshotStore store;
  daw::SnapshotError seed;
  publish(store, &seed, "p1");

  constexpr int kPublishers = 4;
  constexpr int kPerPublisher = 60;
  std::atomic<bool> torn{false};
  std::atomic<bool> stop{false};
  std::atomic<long> observations{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_acquire)) {
        auto snapshot = store.current();
        if (!snapshot) {
          continue;
        }
        observations.fetch_add(1, std::memory_order_relaxed);
        size_t devices = 0;
        for (const auto& plan : snapshot->tracks) {
          devices += plan.devices.size();
          for (const auto& device : plan.devices) {
            const auto owner = snapshot->deviceOwner().find(device.stableDeviceId);
            if (owner == snapshot->deviceOwner().end() || owner->second != plan.trackId) {
              torn.store(true, std::memory_order_release);
            }
          }
          if (plan.hostSegments().pluginNames.size() > plan.devices.size()) {
            torn.store(true, std::memory_order_release);
          }
        }
        if (devices != snapshot->deviceOwner().size() || snapshot->revision == 0) {
          torn.store(true, std::memory_order_release);
        }
      }
    });
  }

  std::vector<std::thread> publishers;
  for (int w = 0; w < kPublishers; ++w) {
    publishers.emplace_back([&, w]() {
      for (int i = 0; i < kPerPublisher; ++i) {
        daw::SnapshotError e;
        store.publish(authored("w" + std::to_string(w) + "_" + std::to_string(i)), 8,
                      daw::PatcherGraph{}, {}, &e);
      }
    });
  }
  for (auto& t : publishers) t.join();
  stop.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();

  expect(observations.load(std::memory_order_relaxed) > 0,
         "the readers actually observed something — a scan that saw nothing asserts nothing");
  expect(!torn.load(std::memory_order_acquire),
         "no reader ever saw a snapshot whose owner map disagreed with its own tracks");
  expect(store.current() != nullptr && store.revision() == store.current()->revision,
         "and the store's revision is the published object's, because there is only one of them");
  expect(store.revision() == kPublishers * kPerPublisher + 1,
         "every publish succeeded and advanced the revision by exactly one — the store serialises "
         "them, so none was lost and none was reused");

  // THE REVISION READ IS CONSERVATIVE, and this is the direction that matters: it may report a
  // holder stale a moment early, and must never report one current after it has been replaced.
  // Because the revision is stored BEFORE the pointer, a reader in that window sees the new number
  // with the old object — which reads as stale, not as current.
  // NAMED, NOT DERIVED FROM THE THING BEING TESTED. `isCurrent(store.revision())` is
  // `r != 0 && r == r`, which passes for any implementation that published anything at all.
  const uint64_t expected = kPublishers * kPerPublisher + 1;
  expect(store.isCurrent(expected), "the revision every publish added up to is the current one");
  expect(!store.isCurrent(expected - 1), "the one before it is not");
  expect(!store.isCurrent(expected + 1),
         "nor is one that was never published — a holder cannot exist for it");
}

// WHAT THESE TESTS CANNOT SEE, said plainly rather than implied by their passing.
//
// Deleting the writer lock is caught, every run. Weakening the publication's memory order to
// relaxed, or replacing the atomic store with a plain assignment, is NOT — five runs each, all
// green. Those are data races, and a race is not a thing a loop can be relied on to observe: on this
// toolchain `std::atomic_load(shared_ptr*)` takes a mutex whatever order it is given, so the
// weakened orders behave identically here and would not on a platform where they do not.
//
// The instrument for that is a thread sanitiser, not more iterations. This binary has no TSan
// target, so the two orderings are currently asserted by reading the code and by nothing else —
// which is worth writing down, because a green suite otherwise reads as coverage of them.


}  // namespace

int main() {
  nothingIsPublishedUntilSomethingIs();
  publishingStampsTheRevisionTheStoreChose();
  aFailedBuildChangesNothing();
  aStaleHolderRefusesAndACurrentOneDoesNot();
  revisionExhaustionRefusesBeforeBuildingAnything();
  manyPublishersAndManyReadersNeverSeeATornSnapshot();

  if (failures != 0) {
    std::printf("engine_snapshot_store_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("engine_snapshot_store_tests: PASS\n");
  return 0;
}
