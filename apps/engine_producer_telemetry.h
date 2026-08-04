#pragma once
// WHAT PRODUCING A BLOCK COSTS — six counters that are only ever read together.
//
// producer.load is the number this repo judges pipeline changes by. Underruns are too coarse and
// too machine-dependent to compare two runs with; the load is producerBlockUsTotal / blocks /
// blockDurationUs, and it means the same thing on any machine. A run that ends without reporting
// it has lost its own result.
//
// COUNTED, NOT SAMPLED. A sampler that blows the budget on the one block where 64 voices start
// together is exactly the case a periodic sample misses, so every block is timed and the worst one
// is kept. Written only by the producer thread and read by the xrun reporter and the shutdown
// summary, which is why relaxed ordering is enough throughout.
//
// THEY WERE SIX MAIN() LOCALS IN THREE Deps STRUCTS — eighteen member slots for one measurement.
// Nothing has ever read one without the others: the load needs the total AND the count, the
// summary prints all six, and a change that added a seventh would have had to be threaded through
// three structs and their initialisers by hand.
#include <atomic>
#include <cstdint>

namespace daw::engine {

struct ProducerTelemetry {
  // PUBLIC and named exactly as the six locals were, so the bodies that read them did not change.
  std::atomic<uint64_t> producerBlocksTimed{0};
  std::atomic<uint64_t> producerBlockUsTotal{0};
  std::atomic<uint64_t> producerBlockUsMax{0};
  std::atomic<uint64_t> producerSamplerUsTotal{0};
  std::atomic<uint64_t> producerSamplerUsMax{0};
  std::atomic<uint64_t> producerBlocksOverBudget{0};

  // The one derived number, in the one place that knows how it is derived. Returns 0.0 when no
  // block has been timed yet — a load of zero blocks is not a load of zero.
  double load(uint64_t blockDurationUs) const {
    const uint64_t blocks = producerBlocksTimed.load(std::memory_order_relaxed);
    if (blocks == 0 || blockDurationUs == 0) {
      return 0.0;
    }
    return static_cast<double>(producerBlockUsTotal.load(std::memory_order_relaxed)) /
           static_cast<double>(blocks) / static_cast<double>(blockDurationUs);
  }
};

}  // namespace daw::engine
