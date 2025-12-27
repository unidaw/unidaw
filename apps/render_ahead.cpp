#include "apps/render_ahead.h"

#include <algorithm>
#include <chrono>

namespace daw {

AudioBlockRing::AudioBlockRing(int numChannels, int blockSize, int capacity)
    : numChannels_(numChannels),
      blockSize_(blockSize),
      capacity_(capacity),
      data_(static_cast<size_t>(capacity) * numChannels * blockSize, 0.0f),
      sampleStarts_(static_cast<size_t>(capacity), 0) {}

bool AudioBlockRing::beginWrite(float** outputs) {
  const int write = writeIndex_.load(std::memory_order_relaxed);
  const int read = readIndex_.load(std::memory_order_acquire);
  const int next = (write + 1) % capacity_;
  if (next == read) {
    return false;
  }
  const size_t base = static_cast<size_t>(write) * numChannels_ * blockSize_;
  for (int ch = 0; ch < numChannels_; ++ch) {
    outputs[ch] = data_.data() + base + static_cast<size_t>(ch) * blockSize_;
  }
  return true;
}

void AudioBlockRing::endWrite(int64_t sampleStart) {
  const int write = writeIndex_.load(std::memory_order_relaxed);
  sampleStarts_[write] = sampleStart;
  writeIndex_.store((write + 1) % capacity_, std::memory_order_release);
}

bool AudioBlockRing::beginRead(const float** outputs, int64_t& sampleStart) {
  const int read = readIndex_.load(std::memory_order_relaxed);
  const int write = writeIndex_.load(std::memory_order_acquire);
  if (read == write) {
    return false;
  }
  const size_t base = static_cast<size_t>(read) * numChannels_ * blockSize_;
  for (int ch = 0; ch < numChannels_; ++ch) {
    outputs[ch] = data_.data() + base + static_cast<size_t>(ch) * blockSize_;
  }
  sampleStart = sampleStarts_[read];
  return true;
}

void AudioBlockRing::endRead() {
  const int read = readIndex_.load(std::memory_order_relaxed);
  readIndex_.store((read + 1) % capacity_, std::memory_order_release);
}

RenderAheadEngine::RenderAheadEngine(int numChannels, int blockSize, int capacity)
    : ring_(numChannels, blockSize, capacity),
      writePointers_(static_cast<size_t>(numChannels), nullptr),
      readPointers_(static_cast<size_t>(numChannels), nullptr) {}

RenderAheadEngine::~RenderAheadEngine() { stop(); }

void RenderAheadEngine::setRenderCallback(RenderCallback callback) {
  renderCallback_ = std::move(callback);
}

void RenderAheadEngine::start(double sampleRate) {
  if (running_.exchange(true)) {
    return;
  }
  sampleRate_ = sampleRate;
  sampleCounter_ = 0;
  thread_ = std::thread([this] { renderLoop(); });
}

void RenderAheadEngine::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool RenderAheadEngine::readBlock(float* const* outputs,
                                  int numChannels,
                                  int numFrames,
                                  int64_t& sampleStartOut) {
  if (numChannels != ring_.numChannels() || numFrames != ring_.blockSize()) {
    for (int ch = 0; ch < numChannels; ++ch) {
      std::fill(outputs[ch], outputs[ch] + numFrames, 0.0f);
    }
    underruns_.fetch_add(1);
    return false;
  }

  if (!ring_.beginRead(readPointers_.data(), sampleStartOut)) {
    for (int ch = 0; ch < numChannels; ++ch) {
      std::fill(outputs[ch], outputs[ch] + numFrames, 0.0f);
    }
    underruns_.fetch_add(1);
    return false;
  }

  for (int ch = 0; ch < numChannels; ++ch) {
    std::copy(readPointers_[ch], readPointers_[ch] + numFrames, outputs[ch]);
  }
  ring_.endRead();
  return true;
}

int RenderAheadEngine::underrunCount() const {
  return underruns_.load(std::memory_order_relaxed);
}

void RenderAheadEngine::renderLoop() {
  const int numChannels = ring_.numChannels();
  const int numFrames = ring_.blockSize();
  const auto sleepDuration = std::chrono::milliseconds(1);

  while (running_.load(std::memory_order_relaxed)) {
    if (!ring_.beginWrite(writePointers_.data())) {
      std::this_thread::sleep_for(sleepDuration);
      continue;
    }

    if (renderCallback_) {
      renderCallback_(writePointers_.data(), numChannels, numFrames, sampleCounter_);
    } else {
      for (int ch = 0; ch < numChannels; ++ch) {
        std::fill(writePointers_[ch], writePointers_[ch] + numFrames, 0.0f);
      }
    }

    ring_.endWrite(sampleCounter_);
    sampleCounter_ += numFrames;
  }
}

}  // namespace daw
