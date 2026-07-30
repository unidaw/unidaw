// Decode/encode round-trip for the audio-file helpers used by M4 audio clips.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

#include "platform_juce/juce_wrapper.h"

int main() {
  const double rate = 48000.0;
  const uint64_t frames = 4800;  // 0.1 s
  const double freq = 440.0;
  std::vector<float> sine(frames);
  for (uint64_t i = 0; i < frames; ++i) {
    sine[i] = 0.5f * static_cast<float>(
                         std::sin(2.0 * M_PI * freq *
                                  static_cast<double>(i) / rate));
  }

  const auto path =
      (std::filesystem::temp_directory_path() / "daw_audio_io_test.wav").string();
  assert(daw::writeWavMono(path, sine.data(), frames, rate));

  const auto dec = daw::decodeAudioFile(path);
  assert(dec.ok);
  assert(dec.frames == frames);
  assert(std::abs(dec.sampleRate - rate) < 1.0);
  assert(dec.sourceChannels == 1);
  assert(dec.channels[0].size() == frames);

  // 16-bit wav round-trip: samples match within quantization (~1/32768).
  double maxErr = 0.0;
  for (uint64_t i = 0; i < frames; ++i) {
    maxErr = std::max(maxErr,
                      std::abs(static_cast<double>(dec.channels[0][i]) - sine[i]));
  }
  assert(maxErr < 1e-3);

  // A missing file decodes to ok=false rather than crashing.
  const auto bad = daw::decodeAudioFile("/nonexistent/definitely/nope.wav");
  assert(!bad.ok);

  std::filesystem::remove(path);
  std::cout << "audio_io_tests: ok" << std::endl;
  return 0;
}
