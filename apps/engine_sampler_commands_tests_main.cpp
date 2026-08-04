// Tests for apps/engine_sampler_commands.h — the first unit tests of a UI COMMAND in this engine.
//
// Until these handlers were extracted from handleUiEntry there was no way to ask a command a
// question. Reaching SamplerSetFilter meant building an engine, mapping shared memory, writing a
// 40-byte payload into a ring and reading a published region back out — about ten seconds and a
// whole process to find out whether filterType 7 is refused. That is why the REJECT paths, which
// are most of what these handlers do, had no direct coverage: every existing sampler check drives
// the happy path through daw-cli, and a refusal is only visible as an event in a log.
//
// The refusals are exactly where this codebase has been bitten. A wrong payload's default failure
// mode on this wire is a SILENT no-op: the caller sees {"sent": ...} and nothing happens. So these
// tests concentrate on what is REFUSED and on what is left UNCHANGED when it is, which is the half
// no happy-path test can see.
//
// Each test constructs the handler's dependencies directly — a track table, a mutex, and stubs
// that RECORD what the handler asked for. No engine, no shared memory, no ring, no device. The
// whole file runs in milliseconds.
#include "apps/engine_sampler_commands.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

using namespace daw;
using namespace daw::engine;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

namespace {

// What the handler asked its collaborators to do. Recording rather than asserting inline, so a
// test can say "and it did NOT report a rejection", which is the assertion that catches a handler
// that silently does nothing.
struct Recorded {
  struct Reject {
    daw::UiCommandType command;
    daw::UiSamplerRejectReason reason;
    uint32_t trackId;
    uint32_t deviceId;
    uint16_t targetId;
  };
  std::vector<Reject> rejects;
  int refreshes = 0;
};

// A fixture holding everything a handler needs. Kept as one object because the deps struct holds
// REFERENCES: the members must outlive it, and a test that let them dangle would be a very
// confusing way to find that out.
struct Fixture {
  UiShmState shm;
  // ONE OBJECT NOW. trackTable.tracks and trackTable.tracksMutex were never apart in any interface, so they are a
  // TrackTable; the handler takes it whole and the fixture builds it whole.
  TrackTable trackTable;
  // TempoMapProvider has no default constructor — 120 bpm is the fixture tempo, and none of the
  // filter assertions depend on it (SamplerEmitRows and SamplerMarker are the two that read it).
  daw::TempoMapProvider tempo{120.0};
  Recorded rec;

  std::function<void(daw::UiCommandType, daw::UiSamplerRejectReason, uint32_t, uint32_t, uint16_t)>
      rejectFn = [this](daw::UiCommandType c, daw::UiSamplerRejectReason r, uint32_t t,
                        uint32_t d, uint16_t g) {
        rec.rejects.push_back({c, r, t, d, g});
      };
  std::function<void(TrackRuntime&)> refreshFn = [this](TrackRuntime&) { rec.refreshes++; };
  std::function<std::shared_ptr<const daw::SamplerRender>(const daw::SamplerState&, uint32_t,
                                                          uint32_t)>
      rebuildFn = [](const daw::SamplerState&, uint32_t, uint32_t) {
        return std::shared_ptr<const daw::SamplerRender>{};
      };
  std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint16_t, bool,
                     std::optional<daw::EventId>, uint16_t, uint16_t)>
      addNoteFn = [](uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint16_t, bool,
                     std::optional<daw::EventId>, uint16_t, uint16_t) { return true; };

  SamplerCommandDeps deps() {
    return SamplerCommandDeps{shm,      trackTable, tempo,
                              rejectFn, refreshFn,  rebuildFn,   addNoteFn};
  }

  // A track carrying one sampler device per id given. Two devices is what makes "addressed by
  // deviceId" a real observation rather than two identical answers agreeing.
  void addTrack(uint32_t trackId, std::vector<uint32_t> samplerDeviceIds) {
    auto rt = std::make_unique<TrackRuntime>();
    rt->trackId = trackId;
    for (uint32_t did : samplerDeviceIds) {
      daw::Device d;
      d.id = did;
      d.kind = daw::DeviceKind::Sampler;
      d.hasSampler = true;
      d.sampler.modSets.push_back(daw::defaultModSet(1));
      rt->track.chain.devices.push_back(d);
    }
    trackTable.tracks.push_back(std::move(rt));
  }

  uint8_t filterTypeOf(uint32_t trackId, uint32_t deviceId) const {
    for (const auto& d : trackTable.tracks[trackId]->track.chain.devices) {
      if (d.id == deviceId && !d.sampler.modSets.empty()) {
        return d.sampler.modSets[0].filterType;
      }
    }
    return 255;
  }
};

daw::EventEntry entryOf(const daw::UiSamplerFilterPayload& p) {
  daw::EventEntry e{};
  e.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  e.size = sizeof(p);
  std::memcpy(e.payload, &p, sizeof(p));
  return e;
}

daw::UiSamplerFilterPayload filterPayload(uint32_t trackId, uint8_t type,
                                          uint32_t deviceId = 0) {
  daw::UiSamplerFilterPayload p{};
  p.commandType = static_cast<uint16_t>(daw::UiCommandType::SamplerSetFilter);
  p.trackId = trackId;
  p.deviceId = deviceId;
  p.modSetId = 0;
  p.filterType = type;
  return p;
}

void callFilter(Fixture& f, const daw::UiSamplerFilterPayload& p) {
  daw::UiCommandPayload header{};
  header.commandType = static_cast<uint16_t>(daw::UiCommandType::SamplerSetFilter);
  auto deps = f.deps();
  const auto entry = entryOf(p);
  handleSamplerSetFilter(deps, entry, header, daw::UiCommandType::SamplerSetFilter);
}

// ------------------------------------------------------------------ a track that is not there
void testNoSuchTrack() {
  Fixture f;                                  // no trackTable.tracks at all
  callFilter(f, filterPayload(0, 2));
  CHECK(f.rec.rejects.size() == 1);
  if (f.rec.rejects.size() == 1) {
    CHECK(f.rec.rejects[0].reason == daw::UiSamplerRejectReason::NoSuchTrack);
    CHECK(f.rec.rejects[0].command == daw::UiCommandType::SamplerSetFilter);
    CHECK(f.rec.rejects[0].trackId == 0);
  }
  // AND IT DID NOT PRETEND TO WORK. A handler that reported the refusal and then also refreshed
  // would publish a new kit version for an edit that never happened.
  CHECK(f.rec.refreshes == 0);

  // An id past the end of a NON-empty table is the same answer, and is the case an off-by-one
  // would get wrong while the empty table still passed.
  Fixture g;
  g.addTrack(0, {1});
  callFilter(g, filterPayload(7, 2));
  CHECK(g.rec.rejects.size() == 1);
  CHECK(g.rec.refreshes == 0);
}

// ------------------------------------------------------- out of range is REFUSED, not clamped
void testFilterTypeRefusedNotClamped() {
  Fixture f;
  f.addTrack(0, {1});
  const uint8_t before = f.filterTypeOf(0, 1);

  callFilter(f, filterPayload(0, 7));         // 7 is past BP (4)

  CHECK(f.rec.rejects.size() == 1);
  if (f.rec.rejects.size() == 1) {
    CHECK(f.rec.rejects[0].reason == daw::UiSamplerRejectReason::BadValue);
    // The offending value comes back so the caller can see WHICH value was wrong.
    CHECK(f.rec.rejects[0].targetId == 7);
  }
  // THE STATE IS UNTOUCHED. This is the assertion that distinguishes "refused" from "clamped to
  // BP": a clamp would leave filterType at 4 and report nothing, and the caller would have a
  // filter they did not ask for with no way to discover the mistake.
  CHECK(f.filterTypeOf(0, 1) == before);
  CHECK(f.rec.refreshes == 0);

  // 4 (BP) is the last LEGAL value and must be accepted — an off-by-one in the guard would refuse
  // it, and no test of "7 is refused" would notice.
  Fixture g;
  g.addTrack(0, {1});
  callFilter(g, filterPayload(0, 4));
  CHECK(g.rec.rejects.empty());
  CHECK(g.filterTypeOf(0, 1) == 4);
}

// --------------------------------------------------------------------------- the happy path
void testAppliesAndRefreshes() {
  Fixture f;
  f.addTrack(0, {1});
  callFilter(f, filterPayload(0, 2));         // LP24
  CHECK(f.rec.rejects.empty());
  CHECK(f.filterTypeOf(0, 1) == 2);
  // The kit version is bumped through this one funnel, so a handler that edits without refreshing
  // leaves every reader looking at a stale kit.
  CHECK(f.rec.refreshes == 1);
}

// ------------------------------------------------------------------- addressed BY device id
void testAddressedByDeviceId() {
  Fixture f;
  f.addTrack(0, {1, 2});                      // two samplers on one track
  callFilter(f, filterPayload(0, 3, /*deviceId=*/2));
  CHECK(f.rec.rejects.empty());
  CHECK(f.filterTypeOf(0, 2) == 3);
  // The OTHER device must be untouched. With one device on the track, a handler that edits
  // whatever it finds first is indistinguishable from one that addresses correctly.
  CHECK(f.filterTypeOf(0, 1) == 0);

  // deviceId 0 means THE FIRST SAMPLER ON THE TRACK — not "every sampler". The handler breaks out
  // of the device loop as soon as one device applied, and UiSamplerFilterPayload documents the
  // field that way ("0 = the first sampler on the track").
  //
  // I wrote this assertion backwards first, expecting "all of them", and it failed. Worth keeping
  // as an explicit test rather than quietly correcting: with one sampler per track — which every
  // fixture in tools/ has — the two rules are indistinguishable, so nothing else in the suite
  // pins which one is in force. A future edit that dropped the `break` would broadcast a filter
  // change across every sampler on the track and no other test would notice.
  Fixture g;
  g.addTrack(0, {1, 2});
  callFilter(g, filterPayload(0, 1, /*deviceId=*/0));
  CHECK(g.filterTypeOf(0, 1) == 1);
  CHECK(g.filterTypeOf(0, 2) == 0);   // untouched: the loop stopped at the first sampler
}

}  // namespace

int main() {
  testNoSuchTrack();
  testFilterTypeRefusedNotClamped();
  testAppliesAndRefreshes();
  testAddressedByDeviceId();

  if (g_fail == 0) {
    std::printf("engine_sampler_commands_tests: PASS\n");
    return 0;
  }
  std::printf("engine_sampler_commands_tests: FAIL (%d)\n", g_fail);
  return 1;
}
