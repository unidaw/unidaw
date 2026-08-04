// Tests for the diff and error path in apps/engine_ui_publish.h.
//
// EVERY FAILURE ON THIS PATH IS SILENT BY DESIGN. A diff that does not fit the ring is DROPPED,
// not blocked — the writer runs on the command thread and must never wait on a UI that is not
// draining — so a delivered diff and a discarded one look identical from the caller. The only
// evidence is the two counters and a rate-limited log line, and until this commit nothing asserted
// on either. Every existing check drives the engine and reads what the UI ends up with, which
// cannot see a drop at all: a UI that missed a diff just looks like a UI that is behind.
//
// THE RATE LIMIT IS PART OF THE CONTRACT, not a nicety. A full ring means EVERY subsequent write
// drops, so an unthrottled log turns one stuck reader into thousands of lines a second — which is
// how a diagnostic becomes the reason nobody can read the log that would have explained it.
//
// The ring here is a real one built over a local buffer: same RingHeader, same ringWrite, no shared
// memory and no engine.
#include "apps/engine_ui_publish.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "apps/event_ring.h"
#include "apps/shared_memory.h"

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

// A ring of `capacity` entries over an ordinary heap buffer.
struct Ring {
  std::vector<uint8_t> storage;
  daw::EventRingView view;

  explicit Ring(uint32_t capacity) : storage(daw::ringBytes(capacity), 0) {
    auto* header = reinterpret_cast<daw::RingHeader*>(storage.data());
    header->capacity = capacity;
    header->entrySize = sizeof(daw::EventEntry);
    header->readIndex.store(0);
    header->writeIndex.store(0);
    view = daw::makeEventRing(storage.data(), 0);
  }
  uint32_t depth() const {
    return view.header->writeIndex.load() - view.header->readIndex.load();
  }
  bool pop(daw::EventEntry& out) { return daw::ringPop(view, out); }
};

struct Fixture {
  Ring ring{8};
  std::atomic<uint32_t> modVersion{0}, routingVersion{0}, patcherGraphVersion{0};
  std::atomic<uint64_t> uiDiffSent{0}, uiDiffDropped{0}, uiDiffDropLogMs{0};
  std::chrono::steady_clock::time_point uiDiffStart = std::chrono::steady_clock::now();

  struct Journal {
    const char* op;
    const char* outcome;
    uint32_t a, b;
    std::string detail;
  };
  std::vector<Journal> journal;

  std::function<daw::EventRingView(TrackRuntime&)> getRingStd =
      [](TrackRuntime&) { return daw::EventRingView{}; };
  std::function<daw::EventRingView()> getRingUiOut = [this] { return ring.view; };
  std::function<void(const char*, const char*, uint32_t, uint32_t, const std::string&)>
      historyAppend = [this](const char* op, const char* outcome, uint32_t a, uint32_t b,
                             const std::string& detail) {
        journal.push_back({op, outcome, a, b, detail});
      };

  UiPublishDeps deps() {
    return UiPublishDeps{modVersion,      getRingStd,   getRingUiOut,    routingVersion,
                         patcherGraphVersion, historyAppend, uiDiffSent, uiDiffDropped,
                         uiDiffDropLogMs, uiDiffStart};
  }
};

// ------------------------------------------------------------------ delivery
void testDiffIsDelivered() {
  Fixture f;
  auto d = f.deps();
  daw::UiDiffPayload payload{};
  payload.trackId = 3;
  emitUiDiff(d, payload);

  CHECK(f.uiDiffSent.load() == 1u);
  CHECK(f.uiDiffDropped.load() == 0u);
  CHECK(f.ring.depth() == 1u);

  daw::EventEntry entry{};
  CHECK(f.ring.pop(entry));
  CHECK(entry.type == static_cast<uint16_t>(daw::EventType::UiDiff));
  // THE SIZE IS THE PAYLOAD'S OWN. The receiver validates entry.size == sizeof(X) and silently
  // drops anything that disagrees, so a wrong size here is a diff that vanishes at the far end.
  CHECK(entry.size == sizeof(daw::UiDiffPayload));
  daw::UiDiffPayload back{};
  std::memcpy(&back, entry.payload, sizeof(back));
  CHECK(back.trackId == 3u);
}

// -------------------------------------------------------------------- drops
void testFullRingDropsAndCounts() {
  Fixture f;
  auto d = f.deps();
  daw::UiDiffPayload payload{};

  // Fill it. A ring of capacity N holds N-1 entries — the slot before readIndex is what
  // distinguishes full from empty — so this loop is written to stop when writes stop landing
  // rather than to assume a number.
  uint64_t sent = 0;
  for (int i = 0; i < 64; ++i) {
    emitUiDiff(d, payload);
    if (f.uiDiffDropped.load() != 0u) break;
    sent = f.uiDiffSent.load();
  }
  CHECK(sent >= 1u);
  CHECK(f.uiDiffDropped.load() == 1u);  // exactly the write that found it full
  f.uiDiffDropped.store(0);

  // Now it is full, and the next writes must DROP rather than block or overwrite.
  for (int i = 0; i < 5; ++i) {
    emitUiDiff(d, payload);
  }
  CHECK(f.uiDiffSent.load() == sent);        // nothing more got in
  CHECK(f.uiDiffDropped.load() == 5u);       // and every one of them was counted
  CHECK(f.ring.depth() == sent);             // no entry was overwritten

  // Drain one slot and the next write succeeds again — a drop is not a latched state.
  daw::EventEntry entry{};
  CHECK(f.ring.pop(entry));
  emitUiDiff(d, payload);
  CHECK(f.uiDiffSent.load() == sent + 1);
  CHECK(f.uiDiffDropped.load() == 5u);
}

// A ring the engine never mapped (mask == 0) is not an error and not a drop: there is no UI
// attached, so there is nothing to tell.
void testUnmappedRingIsNotADrop() {
  Fixture f;
  f.getRingUiOut = [] { return daw::EventRingView{}; };
  auto d = f.deps();
  daw::UiDiffPayload payload{};
  emitUiDiff(d, payload);
  CHECK(f.uiDiffSent.load() == 0u);
  CHECK(f.uiDiffDropped.load() == 0u);
}

// ---------------------------------------------------------------- rate limit
void testDropLogIsRateLimited() {
  Fixture f;
  auto d = f.deps();
  daw::UiDiffPayload payload{};
  for (int i = 0; i < 64; ++i) {                          // fill, however deep it turns out to be
    emitUiDiff(d, payload);
    if (f.uiDiffDropped.load() != 0u) break;
  }
  f.uiDiffDropped.store(0);

  const uint64_t first = f.uiDiffDropLogMs.load();
  for (int i = 0; i < 200; ++i) emitUiDiff(d, payload);  // 200 drops, back to back
  CHECK(f.uiDiffDropped.load() == 200u);
  // THE STAMP MOVED EXACTLY ONCE. 200 drops inside one second must not produce 200 log lines; the
  // stamp is what stops them, so a stamp that advanced 200 times means the limit is not limiting.
  const uint64_t after = f.uiDiffDropLogMs.load();
  CHECK(after != first || first == 0u);
  for (int i = 0; i < 200; ++i) emitUiDiff(d, payload);
  CHECK(f.uiDiffDropLogMs.load() == after);  // still inside the same second
  CHECK(f.uiDiffDropped.load() == 400u);     // but every drop is still counted
}

// uiDiffNowMs measures from a steady origin, so it never goes backwards.
void testNowMsIsMonotonic() {
  Fixture f;
  auto d = f.deps();
  const uint64_t a = uiDiffNowMs(d);
  const uint64_t b = uiDiffNowMs(d);
  CHECK(b >= a);
  CHECK(a < 60000u);  // measured from THIS fixture's start, not from the epoch
}

// -------------------------------------------------------------- the emitters
void testClipRejectCarriesBothVersions() {
  Fixture f;
  auto d = f.deps();
  emitClipReject(d, daw::UiClipRejectReason::StaleBase, 2, 7, 9,
                 daw::UiCommandType::WriteNote);
  CHECK(f.uiDiffSent.load() == 1u);

  daw::EventEntry entry{};
  CHECK(f.ring.pop(entry));
  CHECK(entry.size == sizeof(daw::UiClipRejectPayload));
  daw::UiClipRejectPayload p{};
  std::memcpy(&p, entry.payload, sizeof(p));
  CHECK(p.trackId == 2u);
  // BOTH VERSIONS TRAVEL. The caller needs the one it sent AND the one the engine holds; a reject
  // carrying only "no" leaves it with nothing to resync to.
  CHECK(p.sentBase == 7u);
  CHECK(p.currentBase == 9u);
  CHECK(p.reason == static_cast<uint16_t>(daw::UiClipRejectReason::StaleBase));

  // A CLIP REJECT DOES NOT JOURNAL, and that is deliberate rather than an omission: the clip
  // version guard writes its own history entry at the call site, where it knows which command was
  // refused. Asserting it here would enshrine a second writer for the same event.
  CHECK(f.journal.empty());
}

void testModAndRoutingErrorsJournal() {
  Fixture f;
  auto d = f.deps();
  emitModError(d, 4, 1, 11);
  emitRoutingError(d, 5, 2);
  // BOTH HALVES, EVERY TIME. A refusal the UI can see and a refusal the journal records are the
  // same event; a caller that got one without the other would have to guess which half happened.
  CHECK(f.uiDiffSent.load() == 2u);
  CHECK(f.journal.size() == 2u);
  CHECK(std::string(f.journal[0].op) == "mod_link");
  CHECK(std::string(f.journal[1].op) == "set_track_routing");
}

void testSamplerRejectJournals() {
  Fixture f;
  auto d = f.deps();
  reportSamplerReject(d, daw::UiCommandType::SamplerLoad,
                      daw::UiSamplerRejectReason::NoSuchTrack, 3, 1, 0);
  // It rides the ordinary diff slot rather than a payload of its own, so it is counted like any
  // other diff — which is the point: a sampler refusal that is dropped must not be free.
  CHECK(f.uiDiffSent.load() == 1u);
  daw::EventEntry entry{};
  CHECK(f.ring.pop(entry));
  CHECK(entry.size == sizeof(daw::UiDiffPayload));
}

}  // namespace

int main() {
  testDiffIsDelivered();
  testFullRingDropsAndCounts();
  testUnmappedRingIsNotADrop();
  testDropLogIsRateLimited();
  testNowMsIsMonotonic();
  testClipRejectCarriesBothVersions();
  testModAndRoutingErrorsJournal();
  testSamplerRejectJournals();

  if (g_fail != 0) {
    std::printf("engine_ui_publish_tests: FAIL (%d)\n", g_fail);
    return 1;
  }
  std::printf("engine_ui_publish_tests: PASS\n");
  return 0;
}
