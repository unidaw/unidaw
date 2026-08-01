// DOES AUDIO WORK ON THIS MACHINE AT ALL?
//
// The smallest program that can answer it: open the default output through the same
// IAudioBackend the engine uses, register a callback that counts and writes a quiet sine, wait,
// and report how many times the device asked for audio.
//
// It exists because the engine could not answer this about itself. On this machine the engine
// opens the device, reports its name, rate and block size, gets TRUE from isPlaying(), and never
// receives a single IO callback — and every layer above reads that as silence with a hundred
// possible causes. Two agents wrote up wrong causes from it. A probe with no producer, no
// threads, no shared memory, no plugin hosts and no transport removes all of them at once: if
// this prints zero, nothing in the DAW is implicated; if it prints thousands, the DAW is.
//
// It also plays a real tone, deliberately. A callback count proves the device is pulling; it does
// not prove anything reaches a speaker, and those are different failures. Anyone running this can
// answer the second half with their ears in three seconds.
//
//   daw_audio_probe [seconds]
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "platform_juce/juce_wrapper.h"

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? std::atof(argv[1]) : 3.0;

  auto runtime = daw::createJuceRuntime();
  auto backend = daw::createAudioBackend();
  if (!backend) {
    std::cout << "audio_probe: no audio backend could be created at all." << std::endl;
    return 2;
  }
  if (!backend->openDefaultDevice(2)) {
    std::cout << "audio_probe: the default output device would NOT OPEN. The device layer says: "
              << backend->lastError() << std::endl;
    return 1;
  }

  std::cout << "audio_probe: opened \"" << backend->deviceName() << "\" at "
            << backend->sampleRate() << " Hz, " << backend->blockSize() << " samples, "
            << backend->outputChannels() << " out / " << backend->inputChannels() << " in"
            << std::endl;

  std::atomic<uint64_t> callbacks{0};
  std::atomic<uint64_t> frames{0};
  const double rate = backend->sampleRate() > 0.0 ? backend->sampleRate() : 44100.0;
  double phase = 0.0;

  // A QUIET SINE, not silence. If the count is non-zero and nothing is audible, the failure is
  // downstream of the callback and worth knowing about separately.
  const bool ok = backend->start([&](float* const* outputs, int numChannels, int numFrames) {
    callbacks.fetch_add(1, std::memory_order_relaxed);
    frames.fetch_add(static_cast<uint64_t>(numFrames), std::memory_order_relaxed);
    const double step = 2.0 * 3.14159265358979323846 * 440.0 / rate;
    for (int i = 0; i < numFrames; ++i) {
      const float s = static_cast<float>(0.08 * std::sin(phase));
      phase += step;
      for (int ch = 0; ch < numChannels; ++ch) {
        if (outputs[ch]) {
          outputs[ch][i] = s;
        }
      }
    }
  });
  if (!ok) {
    std::cout << "audio_probe: start() refused the callback." << std::endl;
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(seconds * 1000)));
  backend->stop();

  const uint64_t n = callbacks.load();
  const uint64_t f = frames.load();
  std::cout << "audio_probe: " << n << " callbacks, " << f << " frames in " << seconds << "s"
            << std::endl;
  std::cout << "  the device reports isPlaying() = " << (backend->isRunning() ? "true" : "false")
            << ", boundary count " << backend->deviceCallbacks() << std::endl;

  if (n == 0) {
    std::cout << "  ZERO. The device accepted being opened and started, and then never asked for"
              << std::endl;
    std::cout << "  audio. Nothing in this program is between the device and that count — no"
              << std::endl;
    std::cout << "  transport, no producer, no plugin host, no shared memory — so this is the OS"
              << std::endl;
    std::cout << "  or the device, and no DAW-side change can affect it." << std::endl;
    if (!backend->lastError().empty()) {
      std::cout << "  the device's own last error: " << backend->lastError() << std::endl;
    }
    return 1;
  }
  const double expected = rate * seconds;
  std::cout << "  the device is pulling audio (" << (100.0 * static_cast<double>(f) / expected)
            << "% of the frames a continuous stream would need). If you heard a 440 Hz tone,"
            << std::endl;
  std::cout << "  the whole output path works and any silence in the DAW is the DAW's."
            << std::endl;
  return 0;
}
