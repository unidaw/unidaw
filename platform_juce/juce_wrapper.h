#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace daw {

struct MidiEvent {
  int sampleOffset = 0;
  uint8_t status = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  uint8_t channel = 0;
  float tuningCents = 0.0f;
  int32_t noteId = 0;
};

using MidiEvents = std::vector<MidiEvent>;
using AudioCallback = std::function<void(float* const* outputs, int numChannels, int numFrames)>;

struct ParamInfo {
  std::string stableId;
  int index = -1;
  std::string name;
  std::string label;
  float defaultNormalized = 0.0f;
  float minValue = 0.0f;
  float maxValue = 1.0f;
  bool isDiscrete = false;
  bool isAutomatable = false;
};

struct PluginScanResult {
  std::string path;
  std::string identifier;
  std::string uid16Hex;
  std::string uid;
  std::string name;
  std::string vendor;
  std::string version;
  std::string format;
  std::string category;
  bool isInstrument = false;
  bool hasEditor = true;
  int numInputChannels = 0;
  int numOutputChannels = 0;
  int paramCount = 0;
  bool ok = false;
  std::string error;
  int64_t scanTimeUnixMs = 0;
};

class IRuntime {
 public:
  virtual ~IRuntime() = default;
};

class IAudioBackend {
 public:
  virtual ~IAudioBackend() = default;

  virtual bool openDefaultDevice(int numOutputs) = 0;
  virtual bool start(AudioCallback callback) = 0;
  virtual void stop() = 0;

  virtual double sampleRate() const = 0;
  virtual int blockSize() const = 0;
  virtual int outputChannels() const = 0;
  virtual std::string deviceName() const = 0;
};

class IPluginInstance {
 public:
  virtual ~IPluginInstance() = default;

  virtual void prepare(double sampleRate, int blockSize, int numOutputs) = 0;
  virtual void process(const float* const* inputs, int numInputs,
                       float* const* outputs, int numOutputs, int numFrames,
                       const MidiEvents& events, int64_t samplePosition) = 0;

  virtual std::string name() const = 0;
  virtual std::string vendor() const = 0;
  virtual std::string identifier() const = 0;
  virtual std::string version() const = 0;
  virtual std::array<uint8_t, 16> pluginUid16() const = 0;
  virtual int numParameters() const = 0;
  virtual int inputChannels() const = 0;
  virtual int outputChannels() const = 0;
  virtual bool loadVst3PresetFile(const std::string& path) = 0;

  virtual const std::vector<ParamInfo>& parameters() const = 0;
  virtual float getParameterValueNormalizedById(const std::string& stableId) const = 0;
  virtual bool setParameterValueNormalizedById(const std::string& stableId, float value) = 0;
  virtual std::string getParameterTextById(const std::string& stableId,
                                           float normalized) const = 0;
  virtual std::vector<uint8_t> getState() const = 0;
  virtual bool setState(const std::vector<uint8_t>& data) = 0;
  virtual void flushParameterChanges() = 0;
  virtual bool openEditor() = 0;
};

class IPluginHost {
 public:
  virtual ~IPluginHost() = default;

  virtual std::unique_ptr<IPluginInstance> loadVst3FromPath(
      const std::string& path, double sampleRate, int blockSize) = 0;
};

std::unique_ptr<IRuntime> createJuceRuntime();
std::unique_ptr<IAudioBackend> createAudioBackend();
std::unique_ptr<IPluginHost> createPluginHost();
std::vector<std::string> discoverVst3Candidates(const std::vector<std::string>& paths);
std::vector<PluginScanResult> scanVst3File(const std::string& path,
                                           bool instantiate,
                                           double sampleRate,
                                           int blockSize);

}  // namespace daw
