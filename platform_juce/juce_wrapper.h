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
  // 0 = continuous. A stepped parameter is a switch with N positions, and a caller that does not
  // know that will write 0.37 to a 5-way selector and get whichever position that lands in.
  int stepCount = 0;
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

// Musical position for the block about to be processed. Without this a hosted
// plugin has no play head, so every tempo-synced delay, arpeggiator and LFO
// free-runs instead of locking to the transport.
struct TransportInfo {
  double bpm = 120.0;
  double ppqPosition = 0.0;
  double ppqPositionOfLastBarStart = 0.0;
  int64_t timeInSamples = 0;
  int timeSigNumerator = 4;
  int timeSigDenominator = 4;
  bool isPlaying = false;
};

// One audio bus of a hosted plugin (Movement 4). A plugin declares an ordered list of
// input and output buses per direction; the main bus is index 0, aux/sidechain/stem
// buses follow. `channelOffset` is the bus's first channel in the flat process buffer,
// so the engine can address a single bus's slice without re-deriving it. `layout` is
// the AudioChannelSet description ("Mono", "Stereo", "5.1", "Ambisonic") so a surround
// bus round-trips as a real channel set, not just a count.
struct BusInfo {
  bool isInput = false;
  int index = 0;
  bool isMain = false;
  bool enabled = false;
  int channelCount = 0;
  int channelOffset = 0;
  uint16_t layoutId = 0;  // stable UiBusLayoutId (0 = discrete/unknown, key on count)
  std::string name;
  std::string layout;     // AudioChannelSet description, for display
};

class IPluginInstance {
 public:
  virtual ~IPluginInstance() = default;

  // enableSidechain / enableAuxOut (Movement 4): enable the plugin's sidechain (aux)
  // INPUT bus (a compressor keyed by another track) and/or its aux OUTPUT buses (a
  // multi-out instrument's stems). Both are off by default (only the main bus enabled)
  // and both govern the negotiated bus layout, so they must be known here.
  virtual void prepare(double sampleRate, int blockSize, int numOutputs,
                       bool enableSidechain = false, bool enableAuxOut = false) = 0;
  // Called before process() for the same block.
  virtual void setTransport(const TransportInfo& transport) = 0;
  // auxOutputs (Movement 4 multi-out): when non-null, the plugin's aux OUTPUT bus
  // channels (everything after the main bus) are written here — numAuxOutputs planar
  // channels of numFrames — so the engine can split a multi-out instrument's stems to
  // child tracks. The main output still goes to `outputs`. Null = main output only.
  virtual void process(const float* const* inputs, int numInputs,
                       float* const* outputs, int numOutputs, int numFrames,
                       const MidiEvents& events, int64_t samplePosition,
                       float* const* auxOutputs = nullptr, int numAuxOutputs = 0) = 0;

  virtual std::string name() const = 0;
  virtual std::string vendor() const = 0;
  virtual std::string identifier() const = 0;
  virtual std::string version() const = 0;
  virtual std::array<uint8_t, 16> pluginUid16() const = 0;
  virtual int numParameters() const = 0;
  virtual int inputChannels() const = 0;
  virtual int outputChannels() const = 0;
  // Samples of processing latency the plugin reports (getLatencySamples), for delay
  // compensation. Valid after prepare().
  virtual int latencySamples() const = 0;
  // The plugin's negotiated bus topology, valid after prepare() (empty before). The
  // engine + UI read this to see and address stems, aux, and sidechain buses.
  virtual std::vector<BusInfo> busLayout() const = 0;
  virtual bool loadVst3PresetFile(const std::string& path) = 0;
  // PANIC: drop the plugin's internal DSP state (voices, delay lines, tails). This is the
  // case a controller message cannot reach — CC120 asks a plugin to stop sounding, and a
  // voice wedged inside its own state ignores it. Defaulted to a no-op so a fixture that
  // holds no state need not implement it.
  virtual void reset() {}

  virtual const std::vector<ParamInfo>& parameters() const = 0;
  virtual float getParameterValueNormalizedById(const std::string& stableId) const = 0;
  virtual bool setParameterValueNormalizedById(const std::string& stableId, float value) = 0;
  virtual std::string getParameterTextById(const std::string& stableId,
                                           float normalized) const = 0;
  virtual std::vector<uint8_t> getState() const = 0;
  virtual bool setState(const std::vector<uint8_t>& data) = 0;
  virtual void flushParameterChanges() = 0;
  virtual bool openEditor() = 0;
  // Keystroke forwarding: the host installs a callback the plugin's editor window invokes
  // with keys the plugin did NOT consume (keyCode, isDown) — press on keydown, release on
  // keyup, for sustained keyjazz. Non-pure so non-JUCE/stub instances ignore it.
  virtual void setKeyForwardCallback(std::function<void(int, bool)>) {}
};

class IPluginHost {
 public:
  virtual ~IPluginHost() = default;

  // `desiredName` selects one plugin out of a multi-plugin VST3 bundle by name;
  // pass "" to take the first type (correct for a single-plugin bundle).
  virtual std::unique_ptr<IPluginInstance> loadVst3FromPath(
      const std::string& path, const std::string& desiredName,
      double sampleRate, int blockSize) = 0;
};

struct WaveformPyramid;  // apps/waveform_pyramid.h — built pre-downmix at decode.

// A decoded audio source, one float buffer per channel. `sampleRate` is the
// source file's own rate (resample at play time against the engine rate).
struct DecodedAudio {
  // PLANAR, one vector per source channel, each `frames` long. This used to be a single mono
  // buffer built by averaging the channels — so a stereo loop dropped into the arrangement PLAYED
  // AS MONO while the waveform drawn above it was per-channel and correct. A display that
  // disagrees with what you hear is the class of bug this codebase exists to remove, and nothing
  // caught it because every audio fixture in the suite is mono.
  std::vector<std::vector<float>> channels;
  uint64_t frames = 0;
  double sampleRate = 0.0;
  uint32_t sourceChannels = 0;
  bool ok = false;
  // The min/max + Q15 pyramid for waveform display, built from the multi-channel
  // buffer before the mono downmix above (a downmix loses out-of-phase energy, so
  // it can't drive an honest waveform). Null only when decode failed.
  std::shared_ptr<const WaveformPyramid> pyramid;
};

// Decodes an audio file (wav/aiff/flac/... — whatever JUCE's basic formats read)
// fully into memory, preserving its channels. `ok` is false if the file can't be read.
// Named for what it does now. The old name said Mono, and that was the bug.
DecodedAudio decodeAudioFile(const std::string& path);

// Writes a mono float buffer as a 16-bit wav. Used by the offline bounce and by
// tests that need a known source. Returns false on any I/O error.
bool writeWavMono(const std::string& path, const float* samples, uint64_t frames,
                  double sampleRate);

std::unique_ptr<IRuntime> createJuceRuntime();
std::unique_ptr<IAudioBackend> createAudioBackend();
std::unique_ptr<IPluginHost> createPluginHost();
std::vector<std::string> discoverVst3Candidates(const std::vector<std::string>& paths);
std::vector<PluginScanResult> scanVst3File(const std::string& path,
                                           bool instantiate,
                                           double sampleRate,
                                           int blockSize);

}  // namespace daw
