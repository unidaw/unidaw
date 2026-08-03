// THE PRODUCER'S WORKER POOL.
//
// Hoisted out of daw_engine_main.cpp because renderTrack holds a `std::unique_ptr<WorkerPool>`
// and could not move into a file of its own while the type it points at was declared inside
// main(). A class that only main() can name confines every one of its users to main() — which is
// how a 15,000-line function keeps growing, one reasonable-looking dependency at a time.
//
// It moved VERBATIM: it referenced nothing from the enclosing scope (checked, not assumed — no
// engineConfig, no tracks, no TrackRuntime), so this is a change of location only.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace daw::engine {

class WorkerPool {
 public:
  explicit WorkerPool(size_t threadCount) {
    if (threadCount == 0) {
      threadCount = 1;
    }
    workers_.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
      workers_.emplace_back([this]() { workerLoop(); });
    }
  }

  ~WorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto& thread : workers_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  void enqueue(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
      pending_++;
    }
    cv_.notify_one();
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&]() { return pending_ == 0; });
  }

 private:
  void workerLoop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_--;
        if (pending_ == 0) {
          done_.notify_all();
        }
      }
    }
  }

  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable done_;
  size_t pending_ = 0;
  bool stopping_ = false;
};

}  // namespace daw::engine
