// M2.18: the command ring must survive several producers writing at once.
//
// The end-to-end shell check (tools/multi_producer_ring_check.sh) runs real daw-cli
// processes, but process startup jitter dwarfs the race window — it passed even against
// the old single-producer ring, so it proves the feature works and proves nothing about
// the race. This test creates the collision on purpose: N threads hammering one ring
// with no pauses, so the interleave that used to lose commands happens thousands of
// times per run.
//
// Two failure modes are checked, because the old code could produce either:
//   LOST      — two producers claim the same slot, one payload is overwritten, and the
//               ring advances once, so a command silently never arrives.
//   TORN      — a consumer reads a slot while a producer is still filling it, giving a
//               header from one command and a payload from another.
// Every entry carries (producer, seq) stamped through all 40 payload bytes, so a torn
// entry is detectable rather than merely suspected.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "apps/event_ring.h"
#include "apps/shared_memory.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

constexpr uint32_t kCapacity = 1024;  // power of two, as makeEventRing requires
constexpr uint32_t kProducers = 8;
constexpr uint32_t kPerProducer = 20000;

// A ring lives in shared memory as [RingHeader][entries...], with the entries starting
// at the 64-byte-aligned end of the header. Reproduce that layout in a heap buffer so
// makeEventRing() finds what it expects.
struct RingBuffer {
  std::vector<uint8_t> bytes;

  RingBuffer() {
    const size_t entriesOffset = daw::alignUp(sizeof(daw::RingHeader), 64);
    bytes.assign(entriesOffset + kCapacity * sizeof(daw::EventEntry), 0);
    auto* header = reinterpret_cast<daw::RingHeader*>(bytes.data());
    header->capacity = kCapacity;
    header->entrySize = sizeof(daw::EventEntry);
    header->readIndex.store(0);
    header->writeIndex.store(0);
  }

  daw::EventRingView view() { return daw::makeEventRing(bytes.data(), 0); }
};

// Stamp an entry so that ANY part of it identifies the whole: the consumer can then
// tell a torn read from a clean one.
daw::EventEntry makeEntry(uint32_t producer, uint32_t seq) {
  daw::EventEntry entry{};
  entry.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  entry.size = 40;
  entry.blockId = producer;
  entry.sampleTime = seq;
  const uint32_t stamp = (producer << 24) ^ (seq * 2654435761u);
  for (uint32_t i = 0; i < 40; i += 4) {
    std::memcpy(entry.payload + i, &stamp, sizeof(stamp));
  }
  return entry;
}

bool entryIsIntact(const daw::EventEntry& entry) {
  const uint32_t producer = entry.blockId;
  const uint32_t seq = static_cast<uint32_t>(entry.sampleTime);
  const uint32_t stamp = (producer << 24) ^ (seq * 2654435761u);
  for (uint32_t i = 0; i < 40; i += 4) {
    uint32_t got = 0;
    std::memcpy(&got, entry.payload + i, sizeof(got));
    if (got != stamp) {
      return false;
    }
  }
  return true;
}

void testConcurrentProducersLoseNothing() {
  RingBuffer buffer;
  auto consumerView = buffer.view();

  std::atomic<bool> producersDone{false};
  std::atomic<uint64_t> written{0};
  std::atomic<uint32_t> stuckProducers{0};
  // A broken ring does not merely lose entries — it wedges: producers spin on a ring
  // that never drains, or the consumer waits on indices that never converge. Without a
  // deadline this test HANGS instead of failing, which in CI is far worse than a
  // failure. Everything below gives up at the deadline and reports what it saw.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  auto expired = [&] { return std::chrono::steady_clock::now() > deadline; };

  // Received counts per (producer, seq). A duplicate is as much a bug as a loss.
  std::vector<std::vector<uint8_t>> seen(kProducers,
                                         std::vector<uint8_t>(kPerProducer, 0));
  uint64_t torn = 0;
  uint64_t duplicated = 0;
  uint64_t received = 0;

  std::thread consumer([&] {
    daw::EventEntry entry{};
    for (;;) {
      if (daw::ringPop(consumerView, entry)) {
        if (!entryIsIntact(entry)) {
          ++torn;
          continue;
        }
        const uint32_t producer = entry.blockId;
        const uint32_t seq = static_cast<uint32_t>(entry.sampleTime);
        if (producer >= kProducers || seq >= kPerProducer) {
          ++torn;
          continue;
        }
        if (seen[producer][seq] != 0) {
          ++duplicated;
        }
        seen[producer][seq] = 1;
        ++received;
        continue;
      }
      // Nothing readable. Only stop once the producers are finished AND the ring has
      // drained — a slot reserved but not yet published reads as "empty" here.
      if (producersDone.load(std::memory_order_acquire)) {
        const uint32_t read =
            consumerView.header->readIndex.load(std::memory_order_acquire);
        const uint32_t write =
            consumerView.header->writeIndex.load(std::memory_order_acquire);
        if (read == write) {
          return;
        }
      }
      if (expired()) {
        return;
      }
      std::this_thread::yield();
    }
  });

  std::vector<std::thread> producers;
  for (uint32_t p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      auto view = buffer.view();
      for (uint32_t seq = 0; seq < kPerProducer; ++seq) {
        const daw::EventEntry entry = makeEntry(p, seq);
        // The ring is much smaller than the run, so a full ring is expected and is not
        // a failure — retry until it drains. What must never happen is a write that
        // reports success and then goes missing.
        bool sent = false;
        while (!(sent = daw::ringWrite(view, entry))) {
          if (expired()) {
            break;
          }
          std::this_thread::yield();
        }
        if (!sent) {
          stuckProducers.fetch_add(1, std::memory_order_relaxed);
          return;  // the ring wedged; the assertions below will report it
        }
        written.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& t : producers) {
    t.join();
  }
  producersDone.store(true, std::memory_order_release);
  consumer.join();

  const uint64_t expected = static_cast<uint64_t>(kProducers) * kPerProducer;
  std::printf("  wrote %llu, received %llu, torn %llu, duplicated %llu\n",
              static_cast<unsigned long long>(written.load()),
              static_cast<unsigned long long>(received),
              static_cast<unsigned long long>(torn),
              static_cast<unsigned long long>(duplicated));

  check(stuckProducers.load() == 0,
        "no producer had to give up on a ring that stopped draining");
  check(written.load() == expected, "every producer completed its writes");
  check(torn == 0, "no entry was read while a producer was still filling it");
  check(duplicated == 0, "no entry was delivered twice");

  uint64_t missing = 0;
  for (uint32_t p = 0; p < kProducers; ++p) {
    for (uint32_t seq = 0; seq < kPerProducer; ++seq) {
      if (seen[p][seq] == 0) {
        ++missing;
      }
    }
  }
  if (missing != 0) {
    std::printf("  %llu of %llu commands were LOST — concurrent producers overwrote "
                "each other's slots\n",
                static_cast<unsigned long long>(missing),
                static_cast<unsigned long long>(expected));
  }
  check(missing == 0, "every command written by every producer arrived exactly once");
}

// The consumer must refuse a slot that has been reserved and not yet published, and a
// caller must be able to retire one whose producer died. Single-threaded and exact.
void testReservedSlotIsNotReadable() {
  RingBuffer buffer;
  auto view = buffer.view();

  // Simulate a producer that reserved slot 0 and died before publishing.
  view.header->writeIndex.store(1, std::memory_order_release);

  daw::EventEntry popped{};
  check(!daw::ringPop(view, popped),
        "an unpublished slot is not readable even though the ring looks non-empty");

  uint32_t stalled = 0xFFFFFFFFu;
  check(daw::ringStalledSlot(view, stalled) && stalled == 0,
        "the stalled slot is reported, so a consumer can tell this from an empty ring");

  daw::ringSkipStalledSlot(view);
  check(!daw::ringStalledSlot(view, stalled),
        "retiring the abandoned slot unwedges the ring");

  // And the ring still works afterwards.
  check(daw::ringWrite(view, makeEntry(3, 7)), "write after recovery");
  check(daw::ringPop(view, popped) && popped.blockId == 3,
        "read after recovery returns the new entry");
}

}  // namespace

int main() {
  testReservedSlotIsNotReadable();
  testConcurrentProducersLoseNothing();
  if (failures == 0) {
    std::printf("event_ring_mpsc_tests: all passed\n");
    return 0;
  }
  std::printf("event_ring_mpsc_tests: %d failure(s)\n", failures);
  return 1;
}
