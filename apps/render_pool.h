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
  void parallelFor(std::size_t count, const std::function<void(std::size_t)>& fn) {
    if (count == 0) {
      return;
    }
    if (m_threads.empty() || count == 1) {
      for (std::size_t i = 0; i < count; ++i) {
        fn(i);
      }
      return;
    }
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_fn = &fn;
      m_count = count;
      m_next.store(0, std::memory_order_relaxed);
      m_remaining.store(count, std::memory_order_relaxed);
      ++m_generation;
    }
    m_workCv.notify_all();
    drain();
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_doneCv.wait(lock, [this] {
        return m_remaining.load(std::memory_order_acquire) == 0;
      });
      m_fn = nullptr;
    }
  }

 private:
  // Claim items until there are none left. Reads m_fn only AFTER establishing that the claimed
  // index is in range, which is what makes a worker that wakes up late harmless: it finds the
  // batch exhausted and returns without ever touching a function object that may already be
  // gone.
  void drain() {
    for (;;) {
      const std::size_t i = m_next.fetch_add(1, std::memory_order_relaxed);
      if (i >= m_count) {
        return;
      }
      (*m_fn)(i);
      if (m_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_doneCv.notify_all();
      }
    }
  }

  void worker() {
    std::uint64_t seen = 0;
    for (;;) {
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
      }
      drain();
    }
  }

  std::vector<std::thread> m_threads;
  std::mutex m_mutex;
  std::condition_variable m_workCv;
  std::condition_variable m_doneCv;
  std::atomic<bool> m_stop{false};
  std::uint64_t m_generation = 0;
  const std::function<void(std::size_t)>* m_fn = nullptr;
  std::size_t m_count = 0;
  std::atomic<std::size_t> m_next{0};
  std::atomic<std::size_t> m_remaining{0};
};

}  // namespace daw
