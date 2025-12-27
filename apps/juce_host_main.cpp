#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <algorithm>
#include <cctype>
#include <sys/wait.h>
#include <unistd.h>

#include "apps/plugin_cache.h"
#include "apps/render_ahead.h"
#include "apps/state_container.h"
#include "platform_juce/juce_wrapper.h"

namespace {

struct NoteSchedule {
  int64_t noteOnSample = 0;
  int64_t noteOffSample = 0;
  bool noteOnSent = false;
  bool noteOffSent = false;
};

bool parsePluginPath(int argc, char** argv, std::string& pathOut) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--plugin") {
      pathOut = argv[i + 1];
      return true;
    }
  }
  return false;
}

bool parsePresetPath(int argc, char** argv, std::string& pathOut) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--preset") {
      pathOut = argv[i + 1];
      return true;
    }
  }
  return false;
}

bool parseRescanFlag(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--rescan") {
      return true;
    }
  }
  return false;
}

int parseDurationMs(int argc, char** argv, int defaultMs) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--duration-ms") {
      const int value = std::atoi(argv[i + 1]);
      if (value > 0) {
        return value;
      }
    }
  }
  return defaultMs;
}

std::vector<std::string> parseScanPaths(int argc, char** argv) {
  std::vector<std::string> paths;
  for (int i = 1; i + 1 < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--scan-path" || arg == "--paths") {
      paths.push_back(argv[i + 1]);
      ++i;
    }
  }
  return paths;
}

int runScannerProcess(const std::vector<std::string>& paths) {
  std::vector<std::string> args;
  args.reserve(4 + paths.size() * 2);
  args.push_back("./juce_scan");
  args.push_back("--out");
  args.push_back("plugin_cache.json");
  for (const auto& path : paths) {
    args.push_back("--paths");
    args.push_back(path);
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid == 0) {
    ::execv(argv[0], argv.data());
    std::perror("execv");
    std::_Exit(127);
  }

  if (pid < 0) {
    return -1;
  }

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string pluginPath;
  if (!parsePluginPath(argc, argv, pluginPath)) {
    std::cerr << "Usage: juce_host --plugin /path/to/synth.vst3 "
              << "[--preset /path/to/preset.vstpreset] "
              << "[--duration-ms 5000]" << std::endl;
    return 1;
  }

  const int durationMs = parseDurationMs(argc, argv, 5000);

  if (parseRescanFlag(argc, argv)) {
    auto scanPaths = parseScanPaths(argc, argv);
    if (scanPaths.empty()) {
      scanPaths.push_back("/Library/Audio/Plug-Ins/VST3");
      if (const char* home = std::getenv("HOME")) {
        scanPaths.push_back(std::string(home) + "/Library/Audio/Plug-Ins/VST3");
      }
    }
    std::cout << "Rescanning plugins..." << std::endl;
    const int result = runScannerProcess(scanPaths);
    if (result != 0) {
      std::cout << "Scan command failed with code " << result << std::endl;
    }
  }

  const auto cache = daw::readPluginCache("plugin_cache.json");
  if (!cache.entries.empty()) {
    std::cout << "Plugin cache (plugin_cache.json)" << std::endl;
    for (const auto& entry : cache.entries) {
      if (entry.scanStatus != daw::ScanStatus::Ok) {
        continue;
      }
      std::cout << "  - " << entry.name << " (" << entry.vendor << ")"
                << " [" << entry.pluginUid16 << "] " << entry.path << std::endl;
    }
  }

  std::string presetPath;
  parsePresetPath(argc, argv, presetPath);

  auto runtime = daw::createJuceRuntime();
  auto audio = daw::createAudioBackend();
  if (!audio->openDefaultDevice(2)) {
    std::cerr << "Failed to open default audio device." << std::endl;
    return 1;
  }

  const double sampleRate = audio->sampleRate();
  const int blockSize = audio->blockSize();
  const int numOutputs = audio->outputChannels();

  auto host = daw::createPluginHost();
  auto plugin = host->loadVst3FromPath(pluginPath, sampleRate, blockSize);
  if (!plugin) {
    std::cerr << "Failed to load VST3 at path: " << pluginPath << std::endl;
    return 1;
  }

  plugin->prepare(sampleRate, blockSize, numOutputs);
  if (!presetPath.empty()) {
    const bool presetLoaded = plugin->loadVst3PresetFile(presetPath);
    std::cout << "Preset load: " << (presetLoaded ? "ok" : "failed") << std::endl;
  }

  std::cout << "Audio device: " << audio->deviceName() << std::endl;
  std::cout << "Sample rate: " << sampleRate << " Hz" << std::endl;
  std::cout << "Block size: " << blockSize << " frames" << std::endl;
  std::cout << "Plugin: " << plugin->name() << " (" << plugin->vendor() << ")"
            << std::endl;
  std::cout << "Plugin identifier: " << plugin->identifier() << std::endl;
  std::cout << "Plugin version: " << plugin->version() << std::endl;
  std::cout << "Plugin parameters: " << plugin->numParameters() << std::endl;
  std::cout << "Plugin output channels: " << plugin->outputChannels() << std::endl;

  const auto& params = plugin->parameters();
  std::cout << "Enumerating parameters (" << params.size() << ")" << std::endl;
  for (const auto& param : params) {
    std::cout << "Param[" << param.index << "] id=" << param.stableId
              << " name=\"" << param.name << "\" label=\"" << param.label
              << "\" default=" << param.defaultNormalized
              << " range=[" << param.minValue << ", " << param.maxValue << "]"
              << " discrete=" << (param.isDiscrete ? "yes" : "no")
              << " automatable=" << (param.isAutomatable ? "yes" : "no")
              << std::endl;
  }

  std::string rampParamId;
  std::string rampParamName;
  for (const auto& param : params) {
    if (!param.isAutomatable || param.isDiscrete) {
      continue;
    }
    std::string lower = param.name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("bypass") != std::string::npos) {
      continue;
    }
    rampParamId = param.stableId;
    rampParamName = param.name;
    break;
  }

  if (!rampParamId.empty()) {
    std::cout << "Ramping param: " << rampParamId << " (" << rampParamName << ")"
              << std::endl;
    const float testValue = 0.75f;
    if (plugin->setParameterValueNormalizedById(rampParamId, testValue)) {
      plugin->flushParameterChanges();
      const float readBack = plugin->getParameterValueNormalizedById(rampParamId);
      std::cout << "Param[" << rampParamId << "] set=" << testValue
                << " readBack=" << readBack
                << " text=\"" << plugin->getParameterTextById(rampParamId, readBack)
                << "\"" << std::endl;
    } else {
      std::cout << "Param[" << rampParamId << "] set failed" << std::endl;
    }
  }

  const auto state = plugin->getState();
  if (!state.empty()) {
    const std::string statePath = "state.bin";
    if (daw::writeStateFile(statePath, plugin->version(), plugin->pluginUid16(), state)) {
      std::cout << "Saved state to " << statePath << " (" << state.size()
                << " bytes)" << std::endl;
    }
  }

  if (!state.empty()) {
    std::vector<uint8_t> loaded;
    std::string version;
    std::array<uint8_t, 16> uid16{};
    if (daw::readStateFile("state.bin", version, uid16, loaded)) {
      const bool uidMatches = uid16 == plugin->pluginUid16();
      if (!uidMatches) {
        std::cout << "State header mismatch: uid16 does not match" << std::endl;
      }
      const bool restored = plugin->setState(loaded);
      std::cout << "Reloaded state: " << (restored ? "ok" : "failed") << std::endl;
    } else {
      std::cout << "Reloaded state: failed to read" << std::endl;
    }
  }

  if (!state.empty()) {
    auto verify = host->loadVst3FromPath(pluginPath, sampleRate, blockSize);
    if (verify) {
      verify->prepare(sampleRate, blockSize, numOutputs);
      const bool restored = verify->setState(state);
      float value = 0.0f;
      if (!rampParamId.empty()) {
        value = verify->getParameterValueNormalizedById(rampParamId);
      }
      std::cout << "Roundtrip verify: "
                << (restored ? "state ok" : "state failed")
                << " rampParam=" << value << std::endl;
    } else {
      std::cout << "Roundtrip verify: failed to create second instance" << std::endl;
    }
  }

  std::atomic<int64_t> sampleCounter{0};
  std::atomic<int> blockCounter{0};
  NoteSchedule schedule;
  schedule.noteOnSample = static_cast<int64_t>(sampleRate * 0.25);
  schedule.noteOffSample = schedule.noteOnSample + static_cast<int64_t>(sampleRate * 4.0);

  const int channel = 0;
  const uint8_t note = 60;
  const uint8_t velocity = 100;

  daw::RenderAheadEngine renderAhead(numOutputs, blockSize, 6);
  std::atomic<int> underrunLast{0};

  auto renderCallback = [&](float* const* outputs, int numChannels, int numFrames,
                            int64_t blockStart) {
    const int64_t blockEnd = blockStart + numFrames;

    daw::MidiEvents events;
    events.reserve(3);

    if (!rampParamId.empty()) {
      const double phase = static_cast<double>(blockStart) / sampleRate;
      const float ramp = static_cast<float>((std::sin(phase * 0.25) + 1.0) * 0.5);
      if (!plugin->setParameterValueNormalizedById(rampParamId, ramp)) {
        std::cout << "Param[" << rampParamId << "] ramp failed" << std::endl;
      }
    }

    if (blockStart == 0) {
      events.push_back({0, static_cast<uint8_t>(0xC0 | channel), 0, 0});
    }

    if (!schedule.noteOnSent && schedule.noteOnSample >= blockStart &&
        schedule.noteOnSample < blockEnd) {
      const int offset = static_cast<int>(schedule.noteOnSample - blockStart);
      events.push_back({offset, static_cast<uint8_t>(0x90 | channel), note, velocity});
      schedule.noteOnSent = true;
    }

    if (!schedule.noteOffSent && schedule.noteOffSample >= blockStart &&
        schedule.noteOffSample < blockEnd) {
      const int offset = static_cast<int>(schedule.noteOffSample - blockStart);
      events.push_back({offset, static_cast<uint8_t>(0x80 | channel), note, 0});
      schedule.noteOffSent = true;
    }

    plugin->process(nullptr, 0, outputs, numChannels, numFrames, events, blockStart);
  };

  renderAhead.setRenderCallback(renderCallback);
  renderAhead.start(sampleRate);

  auto callback = [&](float* const* outputs, int numChannels, int numFrames) {
    int64_t blockStart = 0;
    const bool ok = renderAhead.readBlock(outputs, numChannels, numFrames, blockStart);
    if (!ok) {
      const int underruns = renderAhead.underrunCount();
      if (underruns != underrunLast.load()) {
        underrunLast.store(underruns);
        std::cout << "Underrun count: " << underruns << std::endl;
      }
    }

    double sumSquares = 0.0;
    int samples = numChannels * numFrames;
    for (int ch = 0; ch < numChannels; ++ch) {
      const float* channelData = outputs[ch];
      for (int i = 0; i < numFrames; ++i) {
        const float sample = channelData[i];
        sumSquares += static_cast<double>(sample) * sample;
      }
    }

    const double rms = samples > 0 ? std::sqrt(sumSquares / samples) : 0.0;
    const int blockIndex = blockCounter.fetch_add(1);
    if (blockIndex % 50 == 0) {
      std::cout << "RMS: " << rms << std::endl;
    }

    sampleCounter.store(blockStart + numFrames);
  };

  if (!audio->start(callback)) {
    std::cerr << "Failed to start audio callback." << std::endl;
    return 1;
  }

  std::cout << "Playing note... duration " << durationMs << "ms" << std::endl;
  const auto startTime = std::chrono::steady_clock::now();
  while (true) {
    const auto elapsed = std::chrono::steady_clock::now() - startTime;
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    if (elapsedMs >= durationMs) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  audio->stop();
  renderAhead.stop();

  return 0;
}
