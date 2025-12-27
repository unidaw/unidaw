#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace daw {

class AudioBlockRing {
 public:
  AudioBlockRing(int numChannels, int blockSize, int capacity);

  bool beginWrite(float** outputs);
  void endWrite(int64_t sampleStart);
  bool beginRead(const float** outputs, int64_t& sampleStart);
  void endRead();

  int numChannels() const { return numChannels_; }
  int blockSize() const { return blockSize_; }

 private:
  int numChannels_ = 0;
  int blockSize_ = 0;
  int capacity_ = 0;
  std::vector<float> data_;
  std::vector<int64_t> sampleStarts_;
  std::atomic<int> readIndex_{0};
  std::atomic<int> writeIndex_{0};
};

class RenderAheadEngine {
 public:
  using RenderCallback = std::function<void(float* const* outputs,
                                            int numChannels,
                                            int numFrames,
                                            int64_t sampleStart)>;

  RenderAheadEngine(int numChannels, int blockSize, int capacity);
  ~RenderAheadEngine();

  void setRenderCallback(RenderCallback callback);
  void start(double sampleRate);
  void stop();

  bool readBlock(float* const* outputs,
                 int numChannels,
                 int numFrames,
                 int64_t& sampleStartOut);

  int underrunCount() const;

 private:
  void renderLoop();

  AudioBlockRing ring_;
  RenderCallback renderCallback_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<int> underruns_{0};
  std::vector<float*> writePointers_;
  std::vector<const float*> readPointers_;
  double sampleRate_ = 0.0;
  int64_t sampleCounter_ = 0;
};

}  // namespace daw
