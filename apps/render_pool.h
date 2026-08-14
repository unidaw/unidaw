#pragma once

// A FIXED POOL FOR THE PRODUCER'S PER-TRACK WORK.
//
// Measured (tools/producer_load_check.sh): the producer's cost is linear at ~118 us per sampler
// track against a 5805 us budget at 256 frames / 44.1 kHz, and at 32 tracks the sampler DSP is
// 93% of everything it does. One thread therefore runs out at roughly 40 sampler tracks — an
// ordinary tracker project, not an exotic one. This is what stops that from being the ceiling.
//
// DELIBERATELY NOT A GENERAL TASK SYSTEM. It does exactly one thing: run a parallel-for over a
// track list and return when every item is finished. No futures, no queues that outlive a call,
// no work that survives the block it belongs to. A block's work must be complete before the
// block is published, so there is nothing for a richer design to buy.
//
// THE CALLING THREAD PARTICIPATES. With one track and four workers the caller would otherwise
// hand off the only item and idle; here it takes items itself, so the pool never makes a small
// block slower than running it inline.
//
// Items are claimed by atomic index rather than pre-divided. Sampler tracks are not equal — one
// with 64 voices ringing costs many times one that is silent — so a fixed split would leave
// threads idling next to a thread doing all the work.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace daw {

class RenderPool {
 public:
  RenderPool() = default;
  ~RenderPool() { stop(); }

  RenderPool(const RenderPool&) = delete;
  RenderPool& operator=(const RenderPool&) = delete;

  // `threads` is the number of ADDITIONAL workers; the caller is always a worker too, so
  // start(0) leaves parallelFor running everything inline.
  void start(unsigned threads) {
    stop();
    m_stop.store(false, std::memory_order_relaxed);
    m_threads.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
      m_threads.emplace_back([this] { worker(); });
    }
  }

  void stop() {
    if (m_threads.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stop.store(true, std::memory_order_relaxed);
      ++m_generation;
    }
    m_workCv.notify_all();
    for (auto& t : m_threads) {
      if (t.joinable()) {
        t.join();
      }
    }
    m_threads.clear();
  }

  std::size_t workerCount() const { return m_threads.size(); }

  // Runs fn(0..count-1), each exactly once, and returns only when all of them have finished.
  // With no workers, or a single item, it runs inline — same result, no synchronisation.
  //
  // ONE CALLER AT A TIME, and nothing here enforces it. Two concurrent calls HANG: each resets the
  // other's m_remaining, and each sees the other's generation in the ticket and returns from its
  // own inline drain having run nothing. True today — the only call is on the producer thread
  // (engine_produce_block.cpp), start() runs once before that thread does, and stop() only from the
  // destructor — but the fix below made this precondition load-bearing where the previous code
  // merely corrupted, so it is written down rather than left to be rediscovered.
  void parallelFor(std::size_t count, const std::function<void(std::size_t)>& fn) {
    if (count == 0) {
      return;
    }
    // THE THIRD CONDITION IS A BOUND, NOT AN OPTIMISATION. The claim index lives in the low 32 bits
    // of m_ticket, so a count that does not fit there would make `index >= count` unable to fire
    // and `ticket + 1` carry OUT of the index half and into the generation half — after which every
    // worker's generation check fails, they all return, m_remaining never reaches zero and
    // parallelFor hangs forever. That is failure mode 3 again, so the fix must not open a new door
    // to it. Unreachable in this product (count is parallelTracks.size(), bounded by kUiMaxTracks =
    // 64) and demonstrated by independent review with the field narrowed to 8 bits: count 200 ran,
    // count 300 hung. Running such a batch inline is correct and costs nothing anyone will meet.
    if (m_threads.empty() || count == 1 || count > 0xffffffffull) {
      for (std::size_t i = 0; i < count; ++i) {
        fn(i);
      }
      return;
    }
    std::uint32_t generation;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_fn = &fn;
      m_count = count;
      generation = ++m_generation;
      // THE TALLY IS ARMED BEFORE THE BATCH OPENS, and the order of these two lines is the whole
      // of the reason. The ticket is what OPENS the batch: a worker that sees this generation in
      // it may immediately claim an item, run it, and decrement m_remaining. Were the tally
      // published second, that decrement could be CLOBBERED by this store — after which
      // m_remaining never reaches zero and `wait(remaining == 0)` never wakes, which is exactly
      // failure mode 3 below, reintroduced by its own fix.
      //
      // Today no worker can reach drain() except through m_mutex, so the interleaving is not
      // reachable and either order would work. That is a property of how workers are woken, not
      // of this function, and it is the kind of precondition that stops being true quietly — an
      // independent review named this comment as asserting the opposite of what the code did.
      m_remaining.store(count, std::memory_order_relaxed);
      m_ticket.store(static_cast<std::uint64_t>(generation) << 32, std::memory_order_release);
    }
    m_workCv.notify_all();
    drain(&fn, count, generation);
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_doneCv.wait(lock, [this] {
        return m_remaining.load(std::memory_order_acquire) == 0;
      });
      m_fn = nullptr;
    }
  }

 private:
  // Claim items for ONE generation until there are none left.
  //
  // EVERYTHING IT NEEDS IS PASSED IN, taken by the caller under the mutex. It reads no shared plain
  // state at all, which is the whole of the fix below.
  //
  // THE BUG THIS REPLACES, because the shape recurs and the old comment here argued the code was
  // safe. It read `m_fn` and `m_count` — both plain members written under the mutex — with no lock,
  // and justified it thus: "reads m_fn only AFTER establishing that the claimed index is in range,
  // which is what makes a worker that wakes up late harmless". That reasons about ONE member. The
  // straggler it describes re-reads `m_count` and re-claims from `m_next`, and the next batch RESETS
  // both. ThreadSanitizer named the write at parallelFor against the read here.
  //
  // A worker is a straggler whenever it is still looping after the batch's last item COMPLETED —
  // the waiter is released by `m_remaining` reaching zero, which says nothing about whether every
  // worker has left. Three distinct failures follow, and the fix had to close all three:
  //
  //   1. NULL DEREFERENCE. parallelFor sets `m_fn = nullptr`, then the next batch writes a new
  //      pointer and resets the index. Those are plain and relaxed writes with no release between
  //      them, so a straggler can observe the reset index (in range) together with the stale
  //      nullptr, and call it.
  //   2. OUT-OF-RANGE ITEM. A straggler holding the OLD count against the NEW batch claims an index
  //      past the new batch's end and calls fn with it — the caller indexes a track vector by that.
  //   3. A HANG, and the worst of the three because it is silent and on the audio path. Those extra
  //      claims each decrement `m_remaining`, which underflows past zero; `wait(remaining == 0)`
  //      then never wakes and the producer stops producing.
  //
  // The generation is packed INTO the claim counter rather than checked beside it, because a
  // separate check is two loads that can straddle a new batch. One atomic makes "which batch is
  // open" and "which item is next" a single indivisible fact: a straggler's compare-exchange sees
  // the generation move and it leaves without touching a function object, a count, or the tally.
  void drain(const std::function<void(std::size_t)>* fn, std::size_t count,
             std::uint32_t generation) {
    for (;;) {
      std::uint64_t ticket = m_ticket.load(std::memory_order_acquire);
      std::size_t index;
      for (;;) {
        if (static_cast<std::uint32_t>(ticket >> 32) != generation) {
          return;  // a newer batch owns the counter; this one is over
        }
        index = static_cast<std::size_t>(ticket & 0xffffffffull);
        if (index >= count) {
          return;
        }
        if (m_ticket.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
          break;
        }
      }
      (*fn)(index);
      if (m_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_doneCv.notify_all();
      }
    }
  }

  void worker() {
    std::uint32_t seen = 0;
    for (;;) {
      const std::function<void(std::size_t)>* fn = nullptr;
      std::size_t count = 0;
      std::uint32_t generation = 0;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_workCv.wait(lock, [this, &seen] {
          return m_stop.load(std::memory_order_relaxed) || m_generation != seen;
        });
        if (m_stop.load(std::memory_order_relaxed)) {
          return;
        }
        // Taken under the lock, so a batch published after this point cannot be missed: the
        // next wait sees m_generation != seen and returns without blocking.
        seen = m_generation;
        // All three under ONE lock hold, so they always describe the same batch. Copying them is
        // what lets drain() run with no shared plain reads.
        fn = m_fn;
        count = m_count;
        generation = m_generation;
      }
      // Null when this worker woke after its batch had already been retired. There is simply
      // nothing to do — the generation check in drain() would reject every claim anyway.
      if (fn != nullptr) {
        drain(fn, count, generation);
      }
    }
  }

  std::vector<std::thread> m_threads;
  std::mutex m_mutex;
  std::condition_variable m_workCv;
  std::condition_variable m_doneCv;
  std::atomic<bool> m_stop{false};
  // Guarded by m_mutex, all three. Never read outside it.
  std::uint32_t m_generation = 0;
  const std::function<void(std::size_t)>* m_fn = nullptr;
  std::size_t m_count = 0;
  // High 32 bits the generation, low 32 the next unclaimed index. One atomic, so a claim cannot
  // straddle a batch boundary. The index half is bounded above by parallelFor's inline path; the
  // generation half advances one per audio block, so 2^32 * 256 / 44100 = 2.49e7 s — **289 days**
  // at 256 frames / 44.1 kHz before it wraps, and a wrap would need a worker asleep across the
  // whole of it.
  //
  // This comment first said 800 days. It was the only figure here I reasoned out instead of
  // computing, among several I had measured, and it was the only one that was wrong — by 2.8x.
  std::atomic<std::uint64_t> m_ticket{0};
  std::atomic<std::size_t> m_remaining{0};
};

}  // namespace daw
