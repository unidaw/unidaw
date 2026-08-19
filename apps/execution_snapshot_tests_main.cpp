// THE SESSION SNAPSHOT: what a caller can author, and what the builder derives.
//
// AE-P1.2 G2-B item 18, R-HOST-PLAN-AUTHORITY and R-STABLE-DEVICE-TARGETS.
//
// WHAT IS NOT TESTED HERE, AND WHY THAT IS THE POINT. An earlier version of this file had four
// tests that edited a derived field of a hand-assembled snapshot — the launch carrier, the
// device-owner map, the routing graph — and asserted the validator refused it. They are gone, and
// nothing replaced them: `buildExecutionSnapshot` is the only way to obtain an ExecutionSnapshot
// and those members are private to it, so "the carrier disagrees with the plan" is no longer a
// state that can be built. A rule you cannot break needs no test; a rule you can break needs one
// that a reviewer cannot get past, and three of them did.
//
// The helper below therefore calls the real builder. The previous helper derived those parts
// itself, which meant every "must be derived" test was asserting agreement its own setup had just
// manufactured — the shape a reviewer named explicitly.

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "apps/execution_snapshot.h"
#include "apps/shared_memory.h"  // kMasterTrackId

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::printf("execution_snapshot_tests: FAIL %s\n", message.c_str());
    ++failures;
  }
}

daw::DevicePlan hosted(uint32_t id, uint32_t compactIndex, const std::string& name) {
  daw::DevicePlan d;
  d.stableDeviceId = id;
  d.kind = daw::DeviceKind::VstEffect;
  d.occupancy = daw::SlotOccupancy::Occupies;
  d.compactIndex = compactIndex;
  d.resolvedPluginPath = "/plugins/" + name + ".vst3";
  d.resolvedPluginName = name;
  return d;
}

daw::DevicePlan unhosted(uint32_t id, daw::SlotOccupancy why) {
  daw::DevicePlan d;
  d.stableDeviceId = id;
  d.kind = why == daw::SlotOccupancy::NotHosted ? daw::DeviceKind::PatcherEvent
                                                : daw::DeviceKind::VstEffect;
  d.occupancy = why;
  d.compactIndex = daw::kNoCompactIndex;
  return d;
}

// THE HELPER SUPPLIES A MASTER, and that is a precondition of every session rather than of any one
// test: the engine creates the master before a snapshot can exist. A test that is ABOUT the master
// calls the builder directly, so the helper is never the thing deciding the answer.
daw::AuthoredTrackPlan masterPlan(uint32_t trackId = 900) {
  daw::AuthoredTrackPlan plan;
  plan.trackId = trackId;
  plan.isMaster = true;
  return plan;
}

std::optional<daw::ExecutionSnapshot> build(std::vector<daw::AuthoredTrackPlan> tracks,
                                            uint64_t revision, uint32_t nextDeviceId,
                                            daw::SnapshotError* error,
                                            const daw::ExecutionSnapshot* previous = nullptr,
                                            std::vector<uint32_t> registeredInputIds = {7},
                                            daw::PatcherGraph graph = {}) {
  const bool haveMaster = std::any_of(tracks.begin(), tracks.end(),
                                      [](const daw::AuthoredTrackPlan& p) { return p.isMaster; });
  if (!haveMaster) {
    tracks.push_back(masterPlan());
  }
  return daw::buildExecutionSnapshot(revision, nextDeviceId, std::move(tracks), std::move(graph),
                                     registeredInputIds, previous, error);
}

daw::PatcherNode poolNode(uint32_t id, uint32_t ownerDeviceId = 0) {
  daw::PatcherNode node;
  node.id = id;
  node.ownerDeviceId = ownerDeviceId;
  return node;
}

daw::AuthoredTrackPlan trackWith(uint32_t trackId, std::vector<daw::DevicePlan> devices) {
  daw::AuthoredTrackPlan plan;
  plan.trackId = trackId;
  plan.devices = std::move(devices);
  return plan;   // declares nothing; AuthoredTrackPlan defaults that way now
}

const daw::DevicePlan* deviceIn(const daw::ExecutionSnapshot& s, uint32_t trackId, uint32_t id) {
  for (const auto& plan : s.tracks) {
    if (plan.trackId != trackId) continue;
    for (const auto& d : plan.devices) {
      if (d.stableDeviceId == id) return &d;
    }
  }
  return nullptr;
}

daw::AuthoredTrackPlan auxChild(uint32_t trackId, uint32_t parent, uint32_t bus) {
  daw::AuthoredTrackPlan plan = trackWith(trackId, {});
  plan.isAuxChild = true;
  plan.auxParentTrackId = parent;
  plan.auxBusIndex = bus;
  return plan;
}

// ---------------------------------------------------------------- identity vs address
void aReorderMovesTheAddressAndKeepsTheIdentity() {
  // R-STABLE-DEVICE-TARGETS: reorder "preserve[s] ... targets by project-global stable device id
  // while plan-local compact indexes change." A test that only checked the ids survive would pass an
  // implementation where the indexes never moved — the same conflation, harder to see.
  daw::SnapshotError e;
  auto before = build({trackWith(0, {hosted(7, 0, "eq"), hosted(9, 1, "comp")})}, 1, 10, &e);
  auto after = build({trackWith(0, {hosted(9, 0, "comp"), hosted(7, 1, "eq")})}, 2, 10, &e,
                     before ? &*before : nullptr);
  expect(before.has_value() && after.has_value(), "both orderings build");
  if (!before || !after) return;

  expect(deviceIn(*before, 0, 7)->compactIndex == 0 && deviceIn(*after, 0, 7)->compactIndex == 1,
         "device 7's ADDRESS moved: 0 before, 1 after");
  expect(deviceIn(*before, 0, 9)->compactIndex == 1 && deviceIn(*after, 0, 9)->compactIndex == 0,
         "and the other device's moved the other way");
  expect(deviceIn(*before, 0, 7)->resolvedPluginName == deviceIn(*after, 0, 7)->resolvedPluginName,
         "while what the device IS followed its id, not its slot");

  expect(before->tracks.front().hostSegments().pluginNames() ==
             std::vector<std::string>({"eq", "comp"}), "the carrier is in chain order before");
  expect(after->tracks.front().hostSegments().pluginNames() ==
             std::vector<std::string>({"comp", "eq"}),
         "and in the NEW chain order after — a carrier ordered by id would load them wrong");
}

void anUnresolvedPluginTakesNoSlotAndShiftsTheOnesBelow() {
  daw::SnapshotError e;
  auto s = build({trackWith(0, {hosted(7, 0, "eq"),
                                unhosted(8, daw::SlotOccupancy::UnresolvedPlugin),
                                hosted(9, 1, "comp")})}, 1, 10, &e);
  expect(s.has_value(), "a chain with an unresolved device in the middle builds");
  if (!s) return;
  expect(s->tracks.front().hostSegments().pluginNames() == std::vector<std::string>({"eq", "comp"}),
         "the unresolved device contributes nothing to the carrier");
  expect(deviceIn(*s, 0, 8)->compactIndex == daw::kNoCompactIndex,
         "it carries NO address — a stale index is how an unloaded device gets addressed as loaded");
  expect(deviceIn(*s, 0, 9)->compactIndex == 1, "and the device below it is at 1, not 2");
}

// ---------------------------------------------------------------- the two monotonic quantities
void theRevisionIsNonzeroAndMonotonic() {
  daw::SnapshotError e;
  auto four = build({trackWith(0, {hosted(7, 0, "eq")})}, 4, 10, &e);
  expect(four.has_value(), "revision 4 builds");
  expect(build({trackWith(0, {hosted(7, 0, "eq")})}, 5, 10, &e, &*four).has_value(),
         "revision 5 over 4 builds");
  expect(!build({trackWith(0, {hosted(7, 0, "eq")})}, 0, 10, &e).has_value(),
         "revision 0 must never build");
  expect(e.code == daw::SnapshotErrorCode::ZeroRevision,
         "...because 0 is what 'no snapshot yet' means, and a consumer can only tell that from a "
         "real revision if 0 is never one");
  expect(!build({trackWith(0, {hosted(7, 0, "eq")})}, 4, 10, &e, &*four).has_value(),
         "the SAME revision must not republish");
  expect(e.code == daw::SnapshotErrorCode::RevisionWentBackwards, "...as non-monotonic");
  expect(!build({trackWith(0, {hosted(7, 0, "eq")})}, 3, 10, &e, &*four).has_value(),
         "and a LOWER revision must not publish");

  // THE MAXIMUM IS REFUSED BY THE BUILDER TOO, not only by the store's ceiling.
  //
  // A guard sweep found this check surviving every test: the store refuses at maxRevision_ (one
  // below the maximum) so it never hands the builder UINT64_MAX, and nothing else called the builder
  // with it. But `buildExecutionSnapshot` is PUBLIC — the store is its only caller today, and a
  // check that guards a public contract needs a test of that contract rather than of its one
  // current caller.
  //
  // Refusing AT the maximum is a deliberate buffer: the next revision would wrap to 0, which
  // ZeroRevision already refuses, so nothing unsound could reach a consumer either way. Refusing one
  // earlier means the wrap is never even computed.
  expect(!build({trackWith(0, {hosted(7, 0, "eq")})}, UINT64_MAX, 10, &e, &*four).has_value(),
         "the maximum revision is refused rather than published");
  expect(e.code == daw::SnapshotErrorCode::RevisionExhausted, "...as exhaustion");
  expect(build({trackWith(0, {hosted(7, 0, "eq")})}, UINT64_MAX - 1, 10, &e, &*four).has_value(),
         "while one below it is the last usable revision");
}

void theWatermarkIsAWatermarkAndNeverGoesBackwards() {
  // R-DEVICE-ID-LIFETIME: the mark "never decreases on load or undo/redo ... and never reuses a
  // deleted id". Two separate questions, and an earlier version answered only the first: is this a
  // plausible mark. A reviewer published 300 and then 5, and every id from 5 up was free again.
  daw::SnapshotError e;
  expect(!build({trackWith(0, {hosted(7, 0, "eq")})}, 1, 0xFFFFFFFFu, &e).has_value(),
         "a mark past the carrier's range is refused — the codebase's own predicate says so");
  expect(e.code == daw::SnapshotErrorCode::WatermarkNotAWatermark, "...as not a watermark");
  expect(!build({}, 1, 0, &e).has_value(), "and one below the first legal id, even with no devices");
  expect(build({}, 1, daw::kStableDeviceIdExhausted, &e).has_value(),
         "the exhausted sentinel IS legal — a session that allocated every id must be describable");

  auto ahead = build({trackWith(0, {hosted(7, 0, "eq")})}, 1, 300, &e);
  expect(ahead.has_value(), "a first snapshot sets the mark");
  expect(!build({trackWith(0, {hosted(7, 0, "eq")})}, 2, 10, &e, &*ahead).has_value(),
         "a later snapshot may not LOWER the mark");
  expect(e.code == daw::SnapshotErrorCode::WatermarkWentBackwards, "...as a watermark regression");
  expect(build({trackWith(0, {hosted(7, 0, "eq")})}, 2, 300, &e, &*ahead).has_value(),
         "holding it steady is fine — a revision that allocated nothing does not move it");
  auto forward = build({trackWith(0, {hosted(7, 0, "eq")})}, 2, 301, &e, &*ahead);
  expect(forward.has_value(), "and raising it is the normal case");

  // UNDO IS THE CASE THIS IS WRITTEN FOR: the document goes back, the mark does not.
  expect(build({trackWith(0, {})}, 3, 301, &e, &*forward).has_value(),
         "undoing a device's creation keeps the mark where the allocation left it");
  expect(!build({trackWith(0, {})}, 3, 8, &e, &*forward).has_value(),
         "and an undo that ALSO lowered it is exactly the reuse the record forbids");
}

// ---------------------------------------------------------------- global identity
void everyDeviceHasExactlyOneOwner() {
  daw::SnapshotError e;
  expect(!build({trackWith(0, {hosted(7, 0, "eq")}), trackWith(1, {hosted(7, 0, "comp")})},
                1, 10, &e).has_value(),
         "one device id claimed by two tracks must be refused");
  expect(e.code == daw::SnapshotErrorCode::DuplicateDeviceId, "...as a duplicate device id");
  expect(!build({trackWith(3, {hosted(7, 0, "eq")}), trackWith(3, {hosted(8, 0, "c")})},
                1, 10, &e).has_value(),
         "two plans for one track id fail");
  expect(e.code == daw::SnapshotErrorCode::DuplicateTrackId, "...as a duplicate track id");

  auto fine = build({trackWith(0, {hosted(7, 0, "eq")}), trackWith(1, {hosted(8, 0, "comp")})},
                    1, 10, &e);
  expect(fine.has_value(), "distinct tracks with distinct devices pass");
  if (!fine) return;
  expect(fine->deviceOwner().size() == 2 && fine->deviceOwner().at(7) == 0 &&
             fine->deviceOwner().at(8) == 1,
         "and the owner map the BUILDER made names both, each to its own track");
}

void deviceIdsAreBoundedByTheirCarrierAndTheMark() {
  daw::SnapshotError e;
  expect(!build({trackWith(0, {hosted(10, 0, "eq")})}, 1, 10, &e).has_value(),
         "an id AT the mark was never allocated; the next allocation would issue it again");
  expect(e.code == daw::SnapshotErrorCode::DeviceIdAtOrAboveWatermark, "...as unallocated");
  expect(build({trackWith(0, {hosted(9, 0, "eq")})}, 1, 10, &e).has_value(), "one below is fine");
  expect(!build({trackWith(0, {hosted(0, 0, "eq")})}, 1, 10, &e).has_value(),
         "device id 0 is not a stable device identity");
  expect(e.code == daw::SnapshotErrorCode::DeviceIdOutOfRange, "...as out of range");
  expect(!build({trackWith(0, {hosted(daw::kStableDeviceIdExhausted, 0, "eq")})}, 1,
                daw::kStableDeviceIdExhausted, &e).has_value(),
         "and an id past the carrier's range is refused rather than truncated");
  expect(e.code == daw::SnapshotErrorCode::DeviceIdOutOfRange, "...as out of range");
}

// ---------------------------------------------------------------- addresses
void compactIndexesAreCompact() {
  daw::SnapshotError e;
  expect(!build({trackWith(0, {hosted(7, 0, "eq"), hosted(9, 2, "comp")})}, 1, 10, &e).has_value(),
         "a GAP means some consumer computed the index differently from this plan");
  expect(e.code == daw::SnapshotErrorCode::CompactIndexNotCompact, "...as non-compact");
  expect(!build({trackWith(0, {hosted(7, 0, "eq"), hosted(9, 0, "comp")})}, 1, 10, &e).has_value(),
         "two occupying devices at ONE index must be refused — it makes two plugins one plugin");
  expect(!build({trackWith(0, {hosted(7, 1, "eq"), hosted(9, 0, "comp")})}, 1, 10, &e).has_value(),
         "and indexes may not run backwards down the chain");

  auto plan = trackWith(0, {hosted(7, 0, "eq"), unhosted(8, daw::SlotOccupancy::NotHosted)});
  plan.devices[1].compactIndex = 1;
  expect(!build({plan}, 1, 10, &e).has_value(),
         "a device that takes no host slot must carry no address");
  expect(e.code == daw::SnapshotErrorCode::CompactIndexOnUnhostedDevice, "...as a stale index");
}

void anUnhostedKindMayNotOccupyASlot() {
  daw::SnapshotError e;
  auto patcher = trackWith(0, {});
  daw::DevicePlan node;
  node.stableDeviceId = 7;
  node.kind = daw::DeviceKind::PatcherEvent;
  node.occupancy = daw::SlotOccupancy::Occupies;
  node.compactIndex = 0;
  patcher.devices.push_back(node);
  expect(!build({patcher}, 1, 10, &e).has_value(),
         "a patcher device occupying a host slot must be refused — it has no plugin to load");
  expect(e.code == daw::SnapshotErrorCode::UnhostedKindOccupiesSlot, "...as an unhosted kind");

  auto pathless = trackWith(0, {});
  daw::DevicePlan empty;
  empty.stableDeviceId = 7;
  empty.kind = daw::DeviceKind::VstEffect;
  empty.occupancy = daw::SlotOccupancy::Occupies;
  empty.compactIndex = 0;
  pathless.devices.push_back(empty);
  expect(!build({pathless}, 1, 10, &e).has_value(),
         "an occupying device with an EMPTY path must be refused");
  expect(e.code == daw::SnapshotErrorCode::OccupyingDeviceHasNoPlugin, "...as no plugin");

  // EACH TERM OF THE OR SEPARATELY. A reviewer deleted one term of the two-term empty-plugin rule
  // and every test still passed; a two-term rule needs two tests or it has as many holes as
  // untested terms.
  auto noName = trackWith(0, {hosted(7, 0, "eq")});
  noName.devices[0].resolvedPluginName.clear();
  expect(!build({noName}, 1, 10, &e).has_value(), "an empty NAME alone is refused");
  auto noPath = trackWith(0, {hosted(7, 0, "eq")});
  noPath.devices[0].resolvedPluginPath.clear();
  expect(!build({noPath}, 1, 10, &e).has_value(), "and an empty PATH alone is refused");
}

// ---------------------------------------------------------------- aux children
void anAuxChildIsAProjectionOfARealParent() {
  daw::SnapshotError e;
  expect(!build({trackWith(0, {}), auxChild(2, 999, 1)}, 1, 10, &e).has_value(),
         "a child naming a parent no plan holds must be refused, not silently produce nothing");
  expect(e.code == daw::SnapshotErrorCode::AuxChildHasNoParent, "...as a missing parent");
  expect(!build({trackWith(0, {}), auxChild(2, 0, 3), auxChild(4, 0, 3)}, 1, 10, &e).has_value(),
         "two children on one {parent, bus} project the parent's edges twice");
  expect(e.code == daw::SnapshotErrorCode::DuplicateAuxChild, "...as a duplicate projection");
  expect(!build({trackWith(0, {}), auxChild(2, 2, 1)}, 1, 10, &e).has_value(),
         "and a child may not be its own parent");
  expect(e.code == daw::SnapshotErrorCode::AuxParentIsItself, "...as a self parent");

  auto withChain = auxChild(2, 0, 1);
  withChain.devices.push_back(hosted(8, 0, "comp"));
  expect(!build({trackWith(0, {hosted(7, 0, "eq")}), withChain}, 1, 10, &e).has_value(),
         "a stem carrying a chain of its own is not a projection of anything");
  expect(e.code == daw::SnapshotErrorCode::AuxChildHoldsDevices, "...as an aux child with devices");

  expect(build({trackWith(0, {}), auxChild(2, 0, 1)}, 1, 10, &e).has_value(),
         "a child of a real parent on its own bus is legal");
}

void anAuxBusIndexMustNameAnOutputBus() {
  // BUS 0 IS THE PARENT'S MAIN OUTPUT, not a stem. A child on it contributed ZERO edges — published,
  // routing nothing, silently: the same "produced nothing at all" outcome the parent-existence rule
  // was written to stop, one field along. A reviewer swept the index and found 0 accepted and
  // 0xFFFFFFFF accepted, the latter naming an output plane that cannot exist.
  daw::SnapshotError e;
  expect(!build({trackWith(0, {}), auxChild(2, 0, 0)}, 1, 10, &e).has_value(),
         "bus 0 is the parent's own output; a child there is a stem of nothing");
  expect(e.code == daw::SnapshotErrorCode::AuxBusIndexOutOfRange, "...as an out-of-range bus");
  expect(!build({trackWith(0, {}), auxChild(2, 0, 0xFFFFFFFFu)}, 1, 10, &e).has_value(),
         "and a bus past the aux plane is refused rather than routed nowhere");
  expect(build({trackWith(0, {}), auxChild(2, 0, 1)}, 1, 10, &e).has_value(), "bus 1 is a stem");
  expect(build({trackWith(0, {}), auxChild(2, 0, daw::kMaxAuxBusIndex)}, 1, 10, &e).has_value(),
         "and so is the last bus the plane can carry");
}

// ---------------------------------------------------------------- one master
void thereIsExactlyOneMaster() {
  daw::SnapshotError e;
  auto a = trackWith(0, {});
  a.isMaster = true;
  auto b = trackWith(1, {});
  b.isMaster = true;
  expect(!build({a, b}, 1, 10, &e).has_value(), "two masters must be refused");
  expect(e.code == daw::SnapshotErrorCode::MultipleMasters, "...as multiple masters");

  auto both = auxChild(1, 0, 1);
  both.isMaster = true;
  expect(!build({trackWith(0, {}), both}, 1, 10, &e).has_value(),
         "the master may not also be a projection of another track");
  expect(e.code == daw::SnapshotErrorCode::MasterIsAlsoAuxChild, "...as master-and-aux-child");
  expect(build({a, trackWith(1, {})}, 1, 10, &e).has_value(),
         "one master is fine — or the refusals above would pass a builder that refused every master");
}

void theMastersOwnLanesAreValidatedToo() {
  // The master was `continue`d out of the routing collection, so all five of its authored lanes
  // could hold anything. A reviewer gave it midi_in = Master — a row the frozen matrix rejects —
  // and it was accepted, while the identical lane on any other track was refused.
  daw::SnapshotError e;
  auto master = trackWith(0, {});
  master.isMaster = true;
  master.routing.midiIn = {daw::TrackRouteKind::Master, 0, 0};
  expect(!build({master, trackWith(1, {})}, 1, 10, &e).has_value(),
         "the master's own lanes obey the routing matrix like everyone else's");
  expect(e.code == daw::SnapshotErrorCode::MasterLaneInvalid, "...named as a master lane fault");

  auto child = auxChild(2, 0, 1);
  child.routing.midiIn = {daw::TrackRouteKind::Master, 0, 0};
  expect(!build({trackWith(0, {}), child}, 1, 10, &e).has_value(),
         "and so does an aux child's — its edges are derived, its lanes are still authored");
}

void aSessionWithoutAMasterCannotExist() {
  // "at most one" was checked and "at least one" was not. The engine creates the master before any
  // snapshot exists, so a snapshot without one describes a session that cannot be — and master
  // execution would have no plan to run. Called through the builder DIRECTLY, because the helper
  // supplies a master and would answer this question for us.
  daw::SnapshotError e;
  auto none = daw::buildExecutionSnapshot(1, 10, {trackWith(0, {})}, daw::PatcherGraph{}, {7},
                                          nullptr, &e);
  expect(!none.has_value(), "a session with no master plan must be refused");
  expect(e.code == daw::SnapshotErrorCode::NoMasterPlan, "...as having no master");

  auto one = daw::buildExecutionSnapshot(1, 10, {trackWith(0, {}), masterPlan()},
                                         daw::PatcherGraph{}, {7}, nullptr, &e);
  expect(one.has_value(), "and one master is what every session has");
}

void theMasterSentinelIsNotATrackId() {
  daw::SnapshotError e;
  auto impostor = trackWith(daw::kMasterTrackId, {});
  expect(!build({impostor}, 1, 10, &e).has_value(),
         "an ordinary track wearing the master's SHM sentinel is a row two layers disagree about");
  expect(e.code == daw::SnapshotErrorCode::TrackIdIsTheMasterSentinel, "...as the master sentinel");

  auto realMaster = trackWith(daw::kMasterTrackId, {});
  realMaster.isMaster = true;
  expect(daw::buildExecutionSnapshot(1, 10, {trackWith(0, {}), realMaster}, daw::PatcherGraph{},
                                     {7}, nullptr, &e).has_value(),
         "while the master itself may of course carry it");
}

void auxIdentityBelongsOnlyToAnAuxChild() {
  daw::SnapshotError e;
  auto stray = trackWith(0, {});
  stray.auxParentTrackId = 4242;
  stray.auxBusIndex = 99;
  expect(!build({stray}, 1, 10, &e).has_value(),
         "a track that is not an aux child may not carry a parent and a bus — the fields are only "
         "read when isAuxChild, so leaving them set is a statement with no reader");
  expect(e.code == daw::SnapshotErrorCode::NonAuxTrackCarriesAuxIdentity, "...as stray aux identity");

  auto justBus = trackWith(0, {});
  justBus.auxBusIndex = 3;
  expect(!build({justBus}, 1, 10, &e).has_value(), "a stray bus alone is refused too");
}

void samplerIdentityIsPresentExactlyWhenTheDeviceIsASampler() {
  daw::SnapshotError e;
  auto claiming = trackWith(0, {hosted(7, 0, "eq")});
  claiming.devices[0].sampler = std::make_shared<const daw::SamplerState>();
  expect(!build({claiming}, 1, 10, &e).has_value(),
         "a VST effect carrying a sampler document is two answers about what the device is");
  expect(e.code == daw::SnapshotErrorCode::SamplerIdentityDisagreesWithKind, "...as a disagreement");

  auto empty = trackWith(0, {});
  daw::DevicePlan sampler;
  sampler.stableDeviceId = 7;
  sampler.kind = daw::DeviceKind::Sampler;
  sampler.occupancy = daw::SlotOccupancy::NotHosted;
  empty.devices.push_back(sampler);
  expect(!build({empty}, 1, 10, &e).has_value(),
         "and a sampler with NO document is a device execution cannot render");
  expect(e.code == daw::SnapshotErrorCode::SamplerIdentityDisagreesWithKind, "...as a disagreement");

  auto good = trackWith(0, {});
  daw::DevicePlan withDoc = sampler;
  withDoc.sampler = std::make_shared<const daw::SamplerState>();
  good.devices.push_back(withDoc);
  auto built = build({good}, 1, 10, &e);
  expect(built.has_value(), "a sampler carrying its document is legal");
  if (!built) return;
  // AND THE IDENTITY IS SHARED, NOT COPIED: successive revisions point at the same document.
  expect(built->tracks.front().devices.front().sampler == withDoc.sampler,
         "the plan holds the SAME document object it was given, so revisions share it");
}

// ---------------------------------------------------------------- targets
void aTargetMustNameADeviceTheSessionHolds() {
  daw::SnapshotError e;
  auto dangling = trackWith(0, {hosted(7, 0, "eq")});
  daw::AutomationTargetPlan entry;
  entry.target = daw::AutomationTarget::device(99999);
  dangling.automationTargets.push_back(entry);
  expect(!build({dangling}, 1, 10, &e).has_value(),
         "an automation target on a device no track holds must be refused");
  expect(e.code == daw::SnapshotErrorCode::TargetNamesNoDeviceInSession, "...as dangling");

  auto mirrored = trackWith(0, {hosted(7, 0, "eq")});
  daw::MirrorTargetPlan mirror;
  mirror.stableDeviceId = 4242;
  mirrored.mirrorTargets.push_back(mirror);
  expect(!build({mirrored}, 1, 10, &e).has_value(),
         "and a MIRROR target is held to the same rule — a separate population needs its own check");
  expect(e.code == daw::SnapshotErrorCode::TargetNamesNoDeviceInSession,
         "...reported as the same fault, by name, not merely as 'refused'");

  auto good = trackWith(0, {hosted(7, 0, "eq")});
  daw::AutomationTargetPlan resolves;
  resolves.target = daw::AutomationTarget::device(7);
  good.automationTargets.push_back(resolves);
  daw::MirrorTargetPlan goodMirror;
  goodMirror.stableDeviceId = 7;
  goodMirror.parameterUid[0] = 1;
  good.mirrorTargets.push_back(goodMirror);
  expect(build({good}, 1, 10, &e).has_value(), "targets on a device the session holds are legal");
}

void mirrorsAreKeyedByDeviceAndParameter() {
  // R-MIRROR-INSTANCE-IDENTITY keys a mirror by "device instance plus parameter uid". The validator
  // keyed it by device alone, so three mirrors on one device with all-zero uids were accepted —
  // the half of the key the struct was widened to carry went unread.
  daw::SnapshotError e;
  auto plan = trackWith(0, {hosted(7, 0, "eq")});
  daw::MirrorTargetPlan a;
  a.stableDeviceId = 7;
  a.parameterUid[0] = 1;
  daw::MirrorTargetPlan b = a;
  plan.mirrorTargets = {a, b};
  expect(!build({plan}, 1, 10, &e).has_value(),
         "two mirrors on one {device, parameter} must be refused");
  expect(e.code == daw::SnapshotErrorCode::DuplicateMirrorTarget, "...as a duplicate mirror");

  auto distinct = trackWith(0, {hosted(7, 0, "eq")});
  daw::MirrorTargetPlan c = a;
  c.parameterUid[0] = 2;
  distinct.mirrorTargets = {a, c};
  expect(build({distinct}, 1, 10, &e).has_value(),
         "two parameters of one device are two mirrors, which is the point of the key");

  auto zeroUid = trackWith(0, {hosted(7, 0, "eq")});
  daw::MirrorTargetPlan blank;
  blank.stableDeviceId = 7;
  zeroUid.mirrorTargets = {blank};
  expect(!build({zeroUid}, 1, 10, &e).has_value(),
         "and an all-zero parameter uid names no parameter");
  expect(e.code == daw::SnapshotErrorCode::MirrorTargetHasNoParameter, "...as no parameter");
}

void aDisabledTargetCarriesItsReasonAndItsOriginalIndex() {
  daw::SnapshotError e;
  auto emptyReason = trackWith(0, {hosted(7, 0, "eq")});
  daw::AutomationTargetPlan entry;
  entry.target = daw::AutomationTarget::disabledLegacy(3, "legacy_compact_unresolvable");
  entry.disabled = daw::DisabledTargetPlan{3, ""};
  emptyReason.automationTargets.push_back(entry);
  expect(!build({emptyReason}, 1, 10, &e).has_value(),
         "a disabled target with an EMPTY reason is the shape without the purpose");
  expect(e.code == daw::SnapshotErrorCode::DisabledTargetHasNoReason, "...as a missing reason");

  auto mismatched = trackWith(0, {hosted(7, 0, "eq")});
  daw::AutomationTargetPlan wrongTag;
  wrongTag.target = daw::AutomationTarget::device(7);
  wrongTag.disabled = daw::DisabledTargetPlan{3, "legacy_compact_unresolvable"};
  mismatched.automationTargets.push_back(wrongTag);
  expect(!build({mismatched}, 1, 10, &e).has_value(),
         "metadata on a target that is NOT tagged disabled is a contradiction");
  expect(e.code == daw::SnapshotErrorCode::DisabledTargetTagMismatch,
         "...and it says the TAG disagrees, not that a reason is missing — different faults send a "
         "reader to different places");

  auto untagged = trackWith(0, {hosted(7, 0, "eq")});
  daw::AutomationTargetPlan noMeta;
  noMeta.target = daw::AutomationTarget::disabledLegacy(3, "legacy_compact_unresolvable");
  untagged.automationTargets.push_back(noMeta);
  expect(!build({untagged}, 1, 10, &e).has_value(), "and a tagged target with no metadata at all");

  // THE TWO COPIES OF THE ORIGINAL INDEX MUST AGREE. The tag carries one and the metadata carries
  // one; a reviewer set them to 3 and 999 and it was accepted, because the passing fixture had
  // always supplied the agreement.
  auto disagreeing = trackWith(0, {hosted(7, 0, "eq")});
  daw::AutomationTargetPlan split;
  split.target = daw::AutomationTarget::disabledLegacy(3, "legacy_compact_unresolvable");
  split.disabled = daw::DisabledTargetPlan{999, "legacy_compact_unresolvable"};
  disagreeing.automationTargets.push_back(split);
  expect(!build({disagreeing}, 1, 10, &e).has_value(),
         "the tag's original index and the metadata's must be the same number");
  expect(e.code == daw::SnapshotErrorCode::DisabledTargetIndexDisagrees, "...as a disagreement");

  auto good = trackWith(0, {hosted(7, 0, "eq")});
  daw::AutomationTargetPlan ok;
  ok.target = daw::AutomationTarget::disabledLegacy(3, "legacy_compact_unresolvable");
  ok.disabled = daw::DisabledTargetPlan{3, "legacy_compact_unresolvable"};
  good.automationTargets.push_back(ok);
  expect(build({good}, 1, 10, &e).has_value(),
         "a tagged target carrying its original index and reason is legal");
}

void everyAutomationTargetIsWellFormed() {
  // `automationTargetIsValid()` existed, was included, and was never called — verbatim the thing the
  // watermark comment congratulates itself on having fixed, one field along. An `All` target
  // carrying a device id and a legacy index is contradictory and was accepted.
  daw::SnapshotError e;
  auto plan = trackWith(0, {hosted(7, 0, "eq")});
  daw::AutomationTargetPlan malformed;
  malformed.target = daw::AutomationTarget::all();
  malformed.target.stableDeviceId = 99999;
  plan.automationTargets.push_back(malformed);
  expect(!build({plan}, 1, 10, &e).has_value(),
         "an All target carrying a device id is malformed and must be refused");
  expect(e.code == daw::SnapshotErrorCode::TargetMalformed, "...as malformed");
}

// ---------------------------------------------------------------- patcher nodes
void patcherNodeMappingsAreUniqueLocallyAndGlobally() {
  daw::SnapshotError e;
  auto twice = trackWith(0, {hosted(7, 0, "eq")});
  twice.devices[0].patcherNodeMapping = {{3, 100}, {3, 101}};
  expect(!build({twice}, 1, 10, &e).has_value(),
         "one local node mapped twice asks for it to be in two places");
  expect(e.code == daw::SnapshotErrorCode::DuplicateLocalPatcherNode, "...as a duplicate local id");

  auto a = trackWith(0, {hosted(7, 0, "eq")});
  a.devices[0].patcherNodeMapping = {{1, 500}};
  auto b = trackWith(1, {hosted(8, 0, "comp")});
  b.devices[0].patcherNodeMapping = {{1, 500}};
  expect(!build({a, b}, 1, 10, &e).has_value(),
         "two devices on one POOLED node would execute each other's");
  expect(e.code == daw::SnapshotErrorCode::DuplicatePooledPatcherNode, "...as a duplicate pooled id");

  auto sentinel = trackWith(0, {hosted(7, 0, "eq")});
  sentinel.devices[0].patcherNodeMapping = {{1, daw::kPatcherInvalidNodeIndex}};
  expect(!build({sentinel}, 1, 10, &e).has_value(),
         "and the 'no node' sentinel is not a pooled node a mapping may name");
  expect(e.code == daw::SnapshotErrorCode::PooledPatcherNodeIsSentinel, "...as the sentinel");

  // THE POSITIVE CASE NEEDS A GRAPH TO ANCHOR TO. It used to pass with an EMPTY patcher graph,
  // which the membership rule now correctly refuses — the mapping named pooled nodes that were not
  // there. That the "legal" case was mapping onto nothing is itself worth noticing: the test was
  // asserting uniqueness of ids that referred to no node at all.
  daw::PatcherGraph pool;
  pool.nodes = {poolNode(500, 7), poolNode(501, 7)};
  pool.topoOrder = {500, 501};
  auto fine = trackWith(0, {hosted(7, 0, "eq")});
  fine.devices[0].patcherNodeMapping = {{1, 500}, {2, 501}};
  expect(build({fine}, 1, 10, &e, nullptr, {7}, pool).has_value(),
         "distinct locals to distinct pooled nodes the graph contains are legal");
}

// ---------------------------------------------------------------- the global patcher graph
void thePatcherGraphIsInternallyConsistent() {
  // The field was added to the type in the same commit whose comment says "a declared field with no
  // rule is not 'not yet used' — it is a hole with a name", and then given no rule. A reviewer
  // published an edge between two nodes that do not exist, a topological order naming one node three
  // times, and no nodes at all — all accepted.
  daw::SnapshotError e;
  // EACH END SEPARATELY. A single edge with BOTH ends dangling is caught by either check alone, so
  // a mutation sweep found each one individually survivable — the case could not tell them apart.
  // Two edges, each bad at one end, can.
  daw::PatcherGraph badSrc;
  badSrc.nodes = {poolNode(1)};
  daw::PatcherEdge srcEdge;
  srcEdge.src.nodeId = 5;   // not a node
  srcEdge.dst.nodeId = 1;   // is a node
  badSrc.edges = {srcEdge};
  expect(!build({trackWith(0, {})}, 1, 10, &e, nullptr, {7}, badSrc).has_value(),
         "an edge whose SOURCE is not a node is a step with no operation");
  expect(e.code == daw::SnapshotErrorCode::PatcherGraphEdgeNamesNoNode, "...as a dangling edge");

  daw::PatcherGraph badDst;
  badDst.nodes = {poolNode(1)};
  daw::PatcherEdge dstEdge;
  dstEdge.src.nodeId = 1;   // is a node
  dstEdge.dst.nodeId = 9;   // not a node
  badDst.edges = {dstEdge};
  expect(!build({trackWith(0, {})}, 1, 10, &e, nullptr, {7}, badDst).has_value(),
         "and an edge whose DESTINATION is not a node delivers to nothing");
  expect(e.code == daw::SnapshotErrorCode::PatcherGraphEdgeNamesNoNode, "...as a dangling edge");

  daw::PatcherGraph repeated;
  repeated.nodes = {poolNode(1), poolNode(1)};
  expect(!build({trackWith(0, {})}, 1, 10, &e, nullptr, {7}, repeated).has_value(),
         "two nodes sharing an id are two answers to one question");
  expect(e.code == daw::SnapshotErrorCode::PatcherGraphNodeIdRepeated, "...as a repeated node id");

  daw::PatcherGraph badOrder;
  badOrder.nodes = {poolNode(1), poolNode(2), poolNode(3)};
  badOrder.topoOrder = {1, 1, 1};
  expect(!build({trackWith(0, {})}, 1, 10, &e, nullptr, {7}, badOrder).has_value(),
         "a topological order naming one node three times skips work and does other work twice — "
         "and it is the RIGHT LENGTH, which is why a size check would miss it");
  expect(e.code == daw::SnapshotErrorCode::PatcherGraphTopoOrderIsNotAPermutation,
         "...as not a permutation");

  daw::PatcherGraph good;
  good.nodes = {poolNode(1), poolNode(2)};
  daw::PatcherEdge ok;
  ok.src.nodeId = 1;
  ok.dst.nodeId = 2;
  good.edges = {ok};
  good.topoOrder = {1, 2};
  expect(build({trackWith(0, {})}, 1, 10, &e, nullptr, {7}, good).has_value(),
         "a graph whose edges and order name its own nodes is legal");
}

void aMappingMustAnchorToTheGraph() {
  // Uniqueness of the pooled id was checked; MEMBERSHIP was not, so a device could map a local node
  // onto a pooled node the graph does not contain — pointing at nothing, which is the
  // resolves-to-nothing no-op this record removes everywhere else.
  daw::SnapshotError e;
  daw::PatcherGraph graph;
  graph.nodes = {poolNode(500, /*ownerDeviceId=*/7)};
  graph.topoOrder = {500};

  auto mapped = trackWith(0, {hosted(7, 0, "eq")});
  mapped.devices[0].patcherNodeMapping = {{1, 999}};
  expect(!build({mapped}, 1, 10, &e, nullptr, {7}, graph).has_value(),
         "a mapping naming a pooled node the graph does not contain must be refused");
  expect(e.code == daw::SnapshotErrorCode::MappedNodeIsNotInTheGraph, "...as not in the graph");

  auto stolen = trackWith(0, {hosted(8, 0, "eq")});
  stolen.devices[0].patcherNodeMapping = {{1, 500}};
  expect(!build({stolen}, 1, 10, &e, nullptr, {7}, graph).has_value(),
         "and a node the POOL says belongs to device 7 may not be claimed by device 8");
  expect(e.code == daw::SnapshotErrorCode::MappedNodeBelongsToAnotherDevice,
         "...as belonging to another device");

  auto correct = trackWith(0, {hosted(7, 0, "eq")});
  correct.devices[0].patcherNodeMapping = {{1, 500}};
  expect(build({correct}, 1, 10, &e, nullptr, {7}, graph).has_value(),
         "a mapping onto a node the pool agrees it owns is legal");
}

// ---------------------------------------------------------------- routing
void theRoutingGraphIsBuiltFromTheAuthoredLanes() {
  daw::SnapshotError e;
  auto a = trackWith(0, {hosted(7, 0, "eq")});
  a.routing.audioOut = {daw::TrackRouteKind::Track, 1, 0};
  auto b = trackWith(1, {});
  b.routing.audioOut = {daw::TrackRouteKind::Master, 0, 0};
  auto s = build({a, b}, 1, 10, &e);
  expect(s.has_value(), "a routed session builds");
  if (!s) return;
  expect(s->routing().edges.size() == 2, "with the two edges its lanes imply");

  auto illegal = trackWith(0, {});
  illegal.routing.midiIn = {daw::TrackRouteKind::Master, 0, 0};
  expect(!build({illegal, trackWith(1, {})}, 1, 10, &e).has_value(),
         "lanes the routing matrix rejects fail the SNAPSHOT, through the compiler");
  expect(e.code == daw::SnapshotErrorCode::RoutingFailed, "...reported as a routing failure");
  expect(e.routing.code == daw::RoutingErrorCode::MasterAsInputSource,
         "carrying the compiler's own refusal, which names the row");
}

void theRegisteredExternalInputsComeFromTheSession() {
  // The set used to be collected from the very declarations being validated, under a comment saying
  // "so a candidate cannot widen the set by declaring one" — declaring one was exactly how it was
  // widened, and it disarmed a rule the routing compiler enforces correctly.
  daw::SnapshotError e;
  auto plan = trackWith(0, {});
  plan.routing.audioIn = {daw::TrackRouteKind::ExternalInput, 0, 777};
  expect(!build({plan, trackWith(1, {})}, 1, 10, &e, nullptr, {7}).has_value(),
         "a lane naming an input the session does not have must be refused");
  expect(e.code == daw::SnapshotErrorCode::RoutingFailed &&
             e.routing.code == daw::RoutingErrorCode::UnregisteredInputId,
         "...as an unregistered input, through the compiler that already knew how to say so");
  expect(build({plan, trackWith(1, {})}, 1, 10, &e, nullptr, {7, 777}).has_value(),
         "and the same lane is legal when the session actually registers 777");
}

// ---------------------------------------------------------------- the carrier's own rule
void theCarrierDerivationIsAssertedNotOnlyCrossChecked() {
  // compileTrackHostSegments is the one place a carrier is made, so its OUTPUT is asserted against
  // values written out by hand. Nothing else can: the builder and any cross-check would both call
  // this function, and a self-consistent wrong rule is invisible to any amount of cross-checking.
  const auto segments = daw::compileTrackHostSegments(
      {hosted(7, 0, "eq"), unhosted(8, daw::SlotOccupancy::UnresolvedPlugin),
       hosted(9, 1, "multiout"), hosted(11, 2, "verb")}, false);
  expect(segments.pluginNames() == std::vector<std::string>({"eq", "multiout", "verb"}),
         "the carrier holds the hosted plugins in chain order");
  expect(segments.auxOutMask == (1u << 1),
         "and the aux-out bit is set for CARRIER slot 1 — the multi-out plugin is third in the "
         "chain and second in the carrier, because the unresolved device takes no slot");
  expect(segments.sidechainMask == 0u, "with no key source bound, no sidechain bit");
  
  // AND EACH SLOT NAMES ITS DEVICE. This is the half the carrier used to discard: it held paths
  // and names in host-slot order and not which device each slot was, so thirteen sites across the
  // engine rebuilt the mapping from the chain plus a resolver plus the filesystem, and four of
  // them rebuilt it wrongly — a bypass sent to another plugin, plugin state saved from the wrong
  // slot, meters on the wrong device.
  //
  // The unresolved device at chain position 1 is what makes this a real assertion rather than a
  // restatement of the chain: carrier slot 1 is device 9, NOT device 8, and every naive walk that
  // has gone wrong in this codebase went wrong by answering 8.
  expect(segments.slots.size() == 3, "one slot per hosted device");
  expect(segments.slots[0].stableDeviceId == 7 && segments.slots[1].stableDeviceId == 9 &&
             segments.slots[2].stableDeviceId == 11,
         "each slot names the device that occupies it, skipping the unresolved one");
  expect(segments.hostIndexOf(7) == 0u && segments.hostIndexOf(9) == 1u &&
             segments.hostIndexOf(11) == 2u,
         "and a device can be asked where it answers");
  expect(!segments.hostIndexOf(8).has_value(),
         "the unresolved device answers NOWHERE — nothing, not a sentinel, because returning one "
         "here is the silent widening that aimed a lane at every plugin on the track");
  expect(!segments.hostIndexOf(999).has_value(), "and neither does a device that is not present");
  
  // THE ACCESSORS AGREE WITH THE SLOTS, because they are derived from them rather than stored
  // beside them. Two stored vectors is what could fall out of step; this cannot.
  expect(segments.pluginPaths().size() == segments.slots.size() &&
             segments.pluginNames().size() == segments.slots.size(),
         "the flat lists a launch sends are exactly as long as the slots they come from");

  expect(daw::compileTrackHostSegments({hosted(7, 0, "multiout"), hosted(9, 1, "eq"),
                                        hosted(11, 2, "multiout")}, false).auxOutMask ==
             ((1u << 0) | (1u << 2)),
         "two multi-out plugins set the bits for their own carrier slots");
  expect(daw::compileTrackHostSegments({hosted(7, 0, "eq")}, false).auxOutMask == 0u,
         "and a chain with none sets no bits at all");

  std::vector<daw::DevicePlan> many;
  for (uint32_t i = 0; i < 33; ++i) many.push_back(hosted(i + 1, i, "multiout"));
  const auto wide = daw::compileTrackHostSegments(many, false);
  expect(wide.auxOutMask == 0xFFFFFFFFu,
         "the first 32 slots set all 32 bits and slot 32 sets nothing — a shift by 32 is undefined "
         "and would fold onto bit 0, putting a stem on the wrong plugin");

  expect(daw::compileTrackHostSegments({hosted(7, 0, "comp")}, true).sidechainMask == 1u,
         "bit 0 keys the first plugin when a source is bound");
  expect(daw::compileTrackHostSegments({}, true).sidechainMask == 0u,
         "and with no plugins there is nothing to key");
}

void bypassDoesNotChangeSlotOccupancy() {
  // Step 2a found forEachHostedDevice and rebuildHostForChain disagreeing about this and settled it:
  // a bypassed plugin still occupies its slot, because the host still holds it.
  daw::SnapshotError e;
  auto plan = trackWith(0, {hosted(7, 0, "eq"), hosted(9, 1, "comp")});
  plan.devices[0].bypass = true;
  auto s = build({plan}, 1, 10, &e);
  expect(s.has_value(), "a chain with a bypassed plugin builds");
  if (!s) return;
  expect(s->tracks.front().hostSegments().pluginNames() == std::vector<std::string>({"eq", "comp"}),
         "the bypassed plugin is STILL in the carrier — the host holds it either way");
  expect(deviceIn(*s, 0, 9)->compactIndex == 1,
         "so the device after it keeps its address: bypass is not a slot change");
}

}  // namespace

int main() {
  aReorderMovesTheAddressAndKeepsTheIdentity();
  anUnresolvedPluginTakesNoSlotAndShiftsTheOnesBelow();
  theRevisionIsNonzeroAndMonotonic();
  theWatermarkIsAWatermarkAndNeverGoesBackwards();
  everyDeviceHasExactlyOneOwner();
  deviceIdsAreBoundedByTheirCarrierAndTheMark();
  compactIndexesAreCompact();
  anUnhostedKindMayNotOccupyASlot();
  anAuxChildIsAProjectionOfARealParent();
  anAuxBusIndexMustNameAnOutputBus();
  thereIsExactlyOneMaster();
  theMastersOwnLanesAreValidatedToo();
  aSessionWithoutAMasterCannotExist();
  theMasterSentinelIsNotATrackId();
  auxIdentityBelongsOnlyToAnAuxChild();
  samplerIdentityIsPresentExactlyWhenTheDeviceIsASampler();
  aTargetMustNameADeviceTheSessionHolds();
  mirrorsAreKeyedByDeviceAndParameter();
  aDisabledTargetCarriesItsReasonAndItsOriginalIndex();
  everyAutomationTargetIsWellFormed();
  patcherNodeMappingsAreUniqueLocallyAndGlobally();
  thePatcherGraphIsInternallyConsistent();
  aMappingMustAnchorToTheGraph();
  theRoutingGraphIsBuiltFromTheAuthoredLanes();
  theRegisteredExternalInputsComeFromTheSession();
  theCarrierDerivationIsAssertedNotOnlyCrossChecked();
  bypassDoesNotChangeSlotOccupancy();

  if (failures != 0) {
    std::printf("execution_snapshot_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("execution_snapshot_tests: PASS\n");
  return 0;
}
