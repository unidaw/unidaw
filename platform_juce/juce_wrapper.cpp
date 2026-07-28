#include "platform_juce/juce_wrapper.h"

#include "apps/waveform_pyramid.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <utility>
#include <unordered_map>

#include <juce_audio_utils/juce_audio_utils.h>
#define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
#include <juce_audio_processors_headless/format_types/juce_VST3Headers.h>
#include <juce_audio_processors_headless/format_types/juce_VST3Utilities.h>
#include <juce_audio_processors_headless/format_types/juce_VST3Common.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/funknown.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/futils.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/ipluginbase.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivstaudioprocessor.h>
#endif

#include "platform_juce/uid_utils.h"

namespace daw {
namespace {

#if JUCE_MAC
template <typename Range>
int hashForRange(Range&& range) noexcept {
  uint32_t value = 0;
  for (const auto& item : range) {
    value = (value * 31U) + static_cast<uint32_t>(item);
  }
  return static_cast<int>(value);
}

std::array<uint32_t, 4> normalisedTuid(const Steinberg::TUID& tuid) noexcept {
  const Steinberg::FUID fuid(tuid);
  return { { static_cast<uint32_t>(fuid.getLong1()),
             static_cast<uint32_t>(fuid.getLong2()),
             static_cast<uint32_t>(fuid.getLong3()),
             static_cast<uint32_t>(fuid.getLong4()) } };
}

std::vector<juce::PluginDescription> buildVst3DescriptionsFromFactory(
    const juce::File& bundleFile,
    std::string* errorOut) {
  std::vector<juce::PluginDescription> descriptions;
  if (!bundleFile.exists()) {
    if (errorOut != nullptr) {
      *errorOut = "Bundle does not exist";
    }
    return descriptions;
  }

  auto* utf8 = bundleFile.getFullPathName().toRawUTF8();
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(utf8),
      static_cast<CFIndex>(std::strlen(utf8)), bundleFile.isDirectory());
  if (url == nullptr) {
    if (errorOut != nullptr) {
      *errorOut = "Failed to create CFURL";
    }
    return descriptions;
  }

  CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
  CFRelease(url);
  if (bundle == nullptr) {
    if (errorOut != nullptr) {
      *errorOut = "CFBundleCreate failed";
    }
    return descriptions;
  }

  CFErrorRef loadError = nullptr;
  const Boolean ok = CFBundleLoadExecutableAndReturnError(bundle, &loadError);
  if (!ok) {
    if (errorOut != nullptr) {
      if (loadError != nullptr) {
        if (auto desc = CFErrorCopyDescription(loadError)) {
          *errorOut = juce::String::fromCFString(desc).toStdString();
          CFRelease(desc);
        } else {
          *errorOut = "CFBundleLoadExecutable failed";
        }
        CFRelease(loadError);
      } else {
        *errorOut = "CFBundleLoadExecutable failed";
      }
    }
    CFRelease(bundle);
    return descriptions;
  }

  using BundleEntryFn = bool (*)(CFBundleRef);
  using BundleExitFn = bool (*)();
  using GetFactoryFn = Steinberg::IPluginFactory* (*)();

  if (auto* entry = reinterpret_cast<BundleEntryFn>(
          CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleEntry")))) {
    if (!entry(bundle)) {
      if (errorOut != nullptr) {
        *errorOut = "bundleEntry failed";
      }
      CFBundleUnloadExecutable(bundle);
      CFRelease(bundle);
      return descriptions;
    }
  }

  auto* getFactory = reinterpret_cast<GetFactoryFn>(
      CFBundleGetFunctionPointerForName(bundle, CFSTR("GetPluginFactory")));
  if (getFactory == nullptr) {
    if (errorOut != nullptr) {
      *errorOut = "GetPluginFactory not found";
    }
    if (auto* exitFn = reinterpret_cast<BundleExitFn>(
            CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleExit")))) {
      exitFn();
    }
    CFBundleUnloadExecutable(bundle);
    CFRelease(bundle);
    return descriptions;
  }

  Steinberg::IPluginFactory* factory = getFactory();
  if (factory == nullptr) {
    if (errorOut != nullptr) {
      *errorOut = "GetPluginFactory returned null";
    }
    if (auto* exitFn = reinterpret_cast<BundleExitFn>(
            CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleExit")))) {
      exitFn();
    }
    CFBundleUnloadExecutable(bundle);
    CFRelease(bundle);
    return descriptions;
  }

  Steinberg::PFactoryInfo factoryInfo{};
  factory->getFactoryInfo(&factoryInfo);
  const juce::String vendor = juce::String(factoryInfo.vendor).trim();

  const Steinberg::int32 classCount = factory->countClasses();
  for (Steinberg::int32 i = 0; i < classCount; ++i) {
    Steinberg::PClassInfo info{};
    factory->getClassInfo(i, &info);
    if (std::strcmp(info.category, kVstAudioEffectClass) != 0) {
      continue;
    }

    juce::PluginDescription desc;
    desc.fileOrIdentifier = bundleFile.getFullPathName();
    desc.lastFileModTime = bundleFile.getLastModificationTime();
    desc.lastInfoUpdateTime = juce::Time::getCurrentTime();
    desc.manufacturerName = vendor;
    desc.name = juce::String(info.name).trim();
    desc.descriptiveName = desc.name;
    desc.pluginFormatName = "VST3";
    desc.numInputChannels = 0;
    desc.numOutputChannels = 0;

    desc.deprecatedUid = hashForRange(info.cid);
    desc.uniqueId = hashForRange(normalisedTuid(info.cid));

    descriptions.push_back(std::move(desc));
  }

  if (auto* exitFn = reinterpret_cast<BundleExitFn>(
          CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleExit")))) {
    exitFn();
  }

  CFBundleUnloadExecutable(bundle);
  CFRelease(bundle);
  return descriptions;
}

void logVst3BundleLoadFailure(const juce::File& bundleFile) {
  if (!bundleFile.exists()) {
    std::cerr << "VST3 bundle does not exist: " << bundleFile.getFullPathName() << std::endl;
    return;
  }

  auto* utf8 = bundleFile.getFullPathName().toRawUTF8();
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(utf8),
      static_cast<CFIndex>(std::strlen(utf8)), bundleFile.isDirectory());
  if (url == nullptr) {
    std::cerr << "Failed to create CFURL for bundle: " << bundleFile.getFullPathName()
              << std::endl;
    return;
  }

  CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
  CFRelease(url);
  if (bundle == nullptr) {
    std::cerr << "CFBundleCreate failed for: " << bundleFile.getFullPathName() << std::endl;
    return;
  }

  CFErrorRef error = nullptr;
  const Boolean ok = CFBundleLoadExecutableAndReturnError(bundle, &error);
  if (ok) {
    CFBundleUnloadExecutable(bundle);
    CFRelease(bundle);
    std::cerr << "CFBundleLoadExecutable succeeded but VST3 creation failed for: "
              << bundleFile.getFullPathName() << std::endl;
    return;
  }

  std::string reason;
  std::string desc;
  if (error != nullptr) {
    if (auto failure = CFErrorCopyFailureReason(error)) {
      reason = juce::String::fromCFString(failure).toStdString();
      CFRelease(failure);
    }
    if (auto description = CFErrorCopyDescription(error)) {
      desc = juce::String::fromCFString(description).toStdString();
      CFRelease(description);
    }
    CFRelease(error);
  }

  std::cerr << "CFBundleLoadExecutable failed for: " << bundleFile.getFullPathName()
            << " reason=\"" << reason << "\" description=\"" << desc << "\"" << std::endl;
  CFRelease(bundle);
}
#endif

class JuceRuntime final : public IRuntime {
 public:
  JuceRuntime() = default;

 private:
  juce::ScopedJuceInitialiser_GUI init_;
};

class JuceAudioBackend final : public IAudioBackend,
                               private juce::AudioIODeviceCallback {
 public:
  bool openDefaultDevice(int numOutputs) override {
    const juce::String error = deviceManager_.initialise(0, numOutputs, nullptr, true);
    if (error.isNotEmpty()) {
      return false;
    }

    if (auto* device = deviceManager_.getCurrentAudioDevice()) {
      sampleRate_ = device->getCurrentSampleRate();
      blockSize_ = device->getCurrentBufferSizeSamples();
      outputChannels_ = device->getActiveOutputChannels().countNumberOfSetBits();
      deviceName_ = device->getName().toStdString();
      return true;
    }

    return false;
  }

  bool start(AudioCallback callback) override {
    if (!callback) {
      return false;
    }

    callback_ = std::move(callback);
    deviceManager_.addAudioCallback(this);
    return true;
  }

  void stop() override {
    deviceManager_.removeAudioCallback(this);
    callback_ = nullptr;
  }

  double sampleRate() const override { return sampleRate_; }
  int blockSize() const override { return blockSize_; }
  int outputChannels() const override { return outputChannels_; }
  std::string deviceName() const override { return deviceName_; }

 private:
  void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
    if (device != nullptr) {
      sampleRate_ = device->getCurrentSampleRate();
      blockSize_ = device->getCurrentBufferSizeSamples();
      outputChannels_ = device->getActiveOutputChannels().countNumberOfSetBits();
      deviceName_ = device->getName().toStdString();
    }
  }

  void audioDeviceStopped() override {}

  void audioDeviceIOCallbackWithContext(const float* const* /*inputChannelData*/,
                                        int /*numInputChannels*/,
                                        float* const* outputChannelData,
                                        int numOutputChannels,
                                        int numSamples,
                                        const juce::AudioIODeviceCallbackContext& /*context*/) override {
    if (numOutputChannels <= 0 || numSamples <= 0) {
      return;
    }

    juce::AudioBuffer<float> buffer(outputChannelData, numOutputChannels, numSamples);
    buffer.clear();

    if (callback_) {
      callback_(outputChannelData, numOutputChannels, numSamples);
    }
  }

  juce::AudioDeviceManager deviceManager_;
  AudioCallback callback_;
  double sampleRate_ = 0.0;
  int blockSize_ = 0;
  int outputChannels_ = 0;
  std::string deviceName_;
};

// Supplies musical position to a hosted plugin. The engine owns the transport,
// so this is a plain holder the host writes before each process call rather
// than a clock of its own.
class EnginePlayHead final : public juce::AudioPlayHead {
 public:
  void update(const TransportInfo& transport) { transport_ = transport; }

  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo info;
    info.setBpm(transport_.bpm);
    info.setPpqPosition(transport_.ppqPosition);
    info.setPpqPositionOfLastBarStart(transport_.ppqPositionOfLastBarStart);
    info.setTimeInSamples(transport_.timeInSamples);
    info.setTimeSignature(
        TimeSignature{transport_.timeSigNumerator, transport_.timeSigDenominator});
    info.setIsPlaying(transport_.isPlaying);
    info.setIsRecording(false);
    info.setIsLooping(false);
    return info;
  }

 private:
  TransportInfo transport_{};
};

// Map a JUCE AudioChannelSet to the stable UiBusLayoutId the UI keys on (see
// shared_memory.h). 0 = discrete/unknown — the UI keys on the channel count there.
static uint16_t busLayoutIdFor(const juce::AudioChannelSet& set) {
  using S = juce::AudioChannelSet;
  if (set == S::mono()) return 1;
  if (set == S::stereo()) return 2;
  if (set == S::createLCR()) return 3;
  if (set == S::createLRS()) return 4;
  if (set == S::quadraphonic()) return 5;
  if (set == S::create5point0()) return 6;
  if (set == S::create5point1()) return 7;
  if (set == S::create6point0()) return 8;
  if (set == S::create6point1()) return 9;
  if (set == S::create7point0()) return 10;
  if (set == S::create7point1()) return 11;
  if (set == S::ambisonic(1)) return 12;
  if (set == S::ambisonic(2)) return 13;
  if (set == S::ambisonic(3)) return 14;
  return 0;
}

class JucePluginInstance final : public IPluginInstance {
 public:
  explicit JucePluginInstance(std::unique_ptr<juce::AudioPluginInstance> instance,
                              std::string vendor,
                              std::string identifier,
                              std::string version)
      : instance_(std::move(instance)),
        vendor_(std::move(vendor)),
        identifier_(std::move(identifier)),
        version_(std::move(version)),
        uid16_(md5Uid16FromIdentifier(identifier_)) {}

  void setTransport(const TransportInfo& transport) override {
    playHead_.update(transport);
  }

  void prepare(double sampleRate, int blockSize, int numOutputs,
               bool enableSidechain = false, bool enableAuxOut = false) override {
    if (!instance_) {
      return;
    }

    instance_->setPlayHead(&playHead_);
    instance_->setNonRealtime(false);

    // Negotiate against the plugin's REAL bus topology (Movement 4 foundation).
    // The old code built a fixed one-input/one-output mono-or-stereo BusesLayout and
    // ignored setBusesLayout's return, so a multi-bus plugin (a multi-out instrument's
    // aux stems, an effect's sidechain input) silently kept its own layout while the
    // host still assumed `numOutputs` channels. Now we start from the plugin's current
    // layout — which has one entry per declared bus — set the MAIN in/out bus to the
    // host width, disable every non-main bus for now (aux outs, sidechain in; later
    // phases enable + route them), and honour the result: if the plugin rejects the
    // layout (e.g. a fixed bus it cannot disable), fall back to its own default so it
    // still runs rather than silently mismatching.
    const juce::AudioChannelSet mainSet = numOutputs == 1
                                              ? juce::AudioChannelSet::mono()
                                              : juce::AudioChannelSet::stereo();
    const bool wantsInput = instance_->getTotalNumInputChannels() > 0;
    juce::AudioProcessor::BusesLayout layout = instance_->getBusesLayout();
    for (int b = 0; b < layout.outputBuses.size(); ++b) {
      // Main bus to host width; aux OUTPUT buses (a multi-out instrument's stems) kept
      // at their own declared layout when requested, else disabled (Movement 4). Opt-in
      // so a normal plugin still presents a single stereo output.
      if (b == 0) {
        layout.outputBuses.getReference(b) = mainSet;
      } else if (!enableAuxOut) {
        layout.outputBuses.getReference(b) = juce::AudioChannelSet::disabled();
      }
    }
    // Movement 4 sidechain: when asked, enable the FIRST aux input bus (index 1) — the
    // conventional sidechain/key input — at the host width. Enabled only on request so
    // a plugin with many input buses (VCV's in[8]) is untouched by default. If the
    // plugin rejects the sidechain layout we retry without it, then fall back, so a
    // plugin that simply has no sidechain still prepares rather than failing.
    for (int b = 0; b < layout.inputBuses.size(); ++b) {
      const bool isMain = b == 0;
      const bool isSidechain = enableSidechain && b == 1;
      layout.inputBuses.getReference(b) =
          ((isMain && wantsInput) || isSidechain) ? mainSet
                                                  : juce::AudioChannelSet::disabled();
    }
    bool layoutApplied = instance_->setBusesLayout(layout);
    if (!layoutApplied && enableSidechain && layout.inputBuses.size() > 1) {
      // The plugin refused the sidechain bus at host width; retry with it disabled so
      // the main path still negotiates (the key input just won't be available).
      layout.inputBuses.getReference(1) = juce::AudioChannelSet::disabled();
      layoutApplied = instance_->setBusesLayout(layout);
    }
    if (!layoutApplied) {
      instance_->enableAllBuses();  // plugin rejected it — keep its own default
    }
    instance_->setPlayConfigDetails(instance_->getTotalNumInputChannels(),
                                    instance_->getTotalNumOutputChannels(),
                                    sampleRate, blockSize);

    // Capture the negotiated bus topology (Movement 4). This is the structured data
    // the engine + UI read to see and address stems/aux/sidechain buses — channelOffset
    // is each bus's first channel in the flat process buffer, and `layout` keeps the
    // AudioChannelSet so a surround bus round-trips as a real channel set. Built once at
    // prepare (off the RT path). A concise line also lands in the host log.
    busLayout_.clear();
    for (int dir = 0; dir < 2; ++dir) {
      const bool isInput = dir == 0;
      const int busCount = instance_->getBusCount(isInput);
      int offset = 0;
      for (int b = 0; b < busCount; ++b) {
        const auto* bus = instance_->getBus(isInput, b);
        BusInfo info;
        info.isInput = isInput;
        info.index = b;
        info.isMain = b == 0;
        info.enabled = bus != nullptr && bus->isEnabled();
        info.channelCount = instance_->getChannelCountOfBus(isInput, b);
        info.channelOffset = offset;  // disabled buses contribute 0, so offset holds
        info.name = bus != nullptr ? bus->getName().toStdString() : std::string();
        info.layout = bus != nullptr
                          ? bus->getCurrentLayout().getDescription().toStdString()
                          : std::string();
        info.layoutId = bus != nullptr
                            ? busLayoutIdFor(bus->getCurrentLayout())
                            : 0;
        busLayout_.push_back(std::move(info));
        offset += info.channelCount;
      }
    }
    {
      std::cerr << "plugin buses: \"" << instance_->getName().toStdString()
                << "\" applied=" << (layoutApplied ? 1 : 0);
      for (const auto& info : busLayout_) {
        std::cerr << " " << (info.isInput ? "in" : "out") << info.index << ":"
                  << info.channelCount << (info.enabled ? "" : "(off)");
      }
      std::cerr << std::endl;
    }
    if (instance_->getNumPrograms() > 0) {
      instance_->setCurrentProgram(0);
    }
    instance_->prepareToPlay(sampleRate, blockSize);
    instance_->suspendProcessing(false);
    instance_->reset();
    pluginOutputs_ = instance_->getTotalNumOutputChannels();
    if (pluginOutputs_ > 0) {
      scratch_.setSize(pluginOutputs_, blockSize, false, false, true);
    }

    if (paramTargetCount_ == 0) {
      buildParameterCache();
    }
  }

  void process(const float* const* inputs, int numInputs,
               float* const* outputs, int numOutputs, int numFrames,
               const MidiEvents& events, int64_t samplePosition,
               float* const* auxOutputs = nullptr, int numAuxOutputs = 0) override {
    if (!instance_ || outputs == nullptr || numOutputs <= 0 || numFrames <= 0) {
      return;
    }

    applyPendingParameterChanges();
    juce::AudioBuffer<float> buffer(outputs, numOutputs, numFrames);
    juce::AudioBuffer<float>* bufferToProcess = &buffer;

    // The process buffer must hold every channel the plugin touches. JUCE lays a plugin
    // out as [main-in, aux-in...] for inputs and [main-out, aux-out...] for outputs,
    // sharing one buffer — so a sidechained effect (main 2 + sidechain 2 in, 2 out)
    // needs a 4-channel buffer with the key signal in channels [2,3]. We scratch it when
    // the plugin's channel count differs from the caller's output array — either extra
    // outputs (a mono plugin on a stereo bus, the pre-existing case) or extra inputs (a
    // sidechain). A plain in-place effect/instrument still processes the output buffer
    // directly, unchanged.
    const int totalIn = instance_ ? instance_->getTotalNumInputChannels() : numInputs;
    const bool outputMismatch = pluginOutputs_ > 0 && pluginOutputs_ != numOutputs;
    const bool hasAuxInput = totalIn > numOutputs;  // sidechain / extra input buses
    if (outputMismatch || hasAuxInput) {
      const int bufChannels =
          std::max({pluginOutputs_ > 0 ? pluginOutputs_ : numOutputs, totalIn, numOutputs});
      scratch_.setSize(bufChannels, numFrames, false, false, true);
      scratch_.clear();
      bufferToProcess = &scratch_;
    }

    if (inputs != nullptr && numInputs > 0) {
      // Copy each provided input channel straight across: channel order already matches
      // the plugin's expected [main..., sidechain...] layout.
      const int channelsToCopy = std::min(numInputs, bufferToProcess->getNumChannels());
      for (int ch = 0; ch < channelsToCopy; ++ch) {
        const float* src = inputs[ch];
        if (src) {
          std::copy(src, src + numFrames, bufferToProcess->getWritePointer(ch));
        }
      }
      if (bufferToProcess == &buffer) {
        for (int ch = channelsToCopy; ch < numOutputs; ++ch) {
          std::fill(buffer.getWritePointer(ch), buffer.getWritePointer(ch) + numFrames,
                    0.0f);
        }
      }
    } else if (bufferToProcess == &buffer) {
      buffer.clear();
    }

    bool useVst3Events = false;
    if (const char* env = std::getenv("DAW_FORCE_VST3_EVENTS")) {
      useVst3Events = std::string(env) == "1";
    } else {
      for (const auto& ev : events) {
        if (ev.tuningCents != 0.0f) {
          useVst3Events = true;
          break;
        }
      }
    }

    if (!useVst3Events ||
        !processWithVst3Events(*bufferToProcess, inputs, numInputs,
                               numOutputs, numFrames, events)) {
      juce::MidiBuffer midi;
      for (const auto& ev : events) {
        const uint8_t status = static_cast<uint8_t>(ev.status | (ev.channel & 0x0F));
        juce::MidiMessage message(status, ev.data1, ev.data2);
        midi.addEvent(message, ev.sampleOffset);
      }
      instance_->processBlock(*bufferToProcess, midi);
    }

    if (bufferToProcess != &buffer) {
      for (int ch = 0; ch < numOutputs; ++ch) {
        auto* dest = buffer.getWritePointer(ch);
        if (ch < pluginOutputs_) {
          const auto* src = bufferToProcess->getReadPointer(ch);
          std::copy(src, src + numFrames, dest);
        } else {
          std::fill(dest, dest + numFrames, 0.0f);
        }
      }
      // Movement 4 multi-out: hand the aux OUTPUT bus channels (everything past the main
      // bus) to the caller. JUCE lays outputs out as [main..., aux...], so the aux buses
      // are the scratch channels after numOutputs; a bus the plugin didn't fill reads
      // silence. Only meaningful when a scratch buffer was used (an aux plugin always
      // scratches, since totalOut > numOutputs).
      if (auxOutputs && numAuxOutputs > 0) {
        const int available =
            std::max(0, bufferToProcess->getNumChannels() - numOutputs);
        for (int ch = 0; ch < numAuxOutputs; ++ch) {
          if (!auxOutputs[ch]) {
            continue;
          }
          if (ch < available && (numOutputs + ch) < pluginOutputs_) {
            const auto* src = bufferToProcess->getReadPointer(numOutputs + ch);
            std::copy(src, src + numFrames, auxOutputs[ch]);
          } else {
            std::fill(auxOutputs[ch], auxOutputs[ch] + numFrames, 0.0f);
          }
        }
      }
    } else if (auxOutputs && numAuxOutputs > 0) {
      // No scratch (plugin has no aux output channels): the aux planes get silence.
      for (int ch = 0; ch < numAuxOutputs; ++ch) {
        if (auxOutputs[ch]) {
          std::fill(auxOutputs[ch], auxOutputs[ch] + numFrames, 0.0f);
        }
      }
    }
  }

  std::string name() const override {
    return instance_ ? instance_->getName().toStdString() : std::string();
  }

  std::string vendor() const override { return vendor_; }

  std::string identifier() const override { return identifier_; }

  std::string version() const override { return version_; }

  std::array<uint8_t, 16> pluginUid16() const override { return uid16_; }

  int numParameters() const override {
    return instance_ ? instance_->getParameters().size() : 0;
  }

  int inputChannels() const override {
    if (!instance_) {
      return 0;
    }
    return instance_->getTotalNumInputChannels();
  }

  int outputChannels() const override { return pluginOutputs_; }

  std::vector<BusInfo> busLayout() const override { return busLayout_; }

  int latencySamples() const override {
    return instance_ ? instance_->getLatencySamples() : 0;
  }

  bool loadVst3PresetFile(const std::string& path) override {
    if (!instance_) {
      return false;
    }

    juce::File file(path);
    if (!file.existsAsFile()) {
      return false;
    }

    juce::MemoryBlock rawData;
    if (!file.loadFileAsData(rawData)) {
      return false;
    }

    struct Visitor final : public juce::ExtensionsVisitor {
      explicit Visitor(const juce::MemoryBlock& dataIn) : data(dataIn) {}

      void visitVST3Client(const VST3Client& vst3) override {
        success = vst3.setPreset(data);
      }

      const juce::MemoryBlock& data;
      bool success = false;
    };

    Visitor visitor(rawData);
    instance_->getExtensions(visitor);
    return visitor.success;
  }

  const std::vector<ParamInfo>& parameters() const override { return params_; }

  float getParameterValueNormalizedById(const std::string& stableId) const override {
    const auto it = paramIdToIndex_.find(stableId);
    if (!instance_ || it == paramIdToIndex_.end()) {
      return 0.0f;
    }
    const int paramIndex = it->second;
    if (paramIndex < 0 || paramIndex >= static_cast<int>(paramPointers_.size())) {
      return 0.0f;
    }
    return paramPointers_[paramIndex]->getValue();
  }

  bool setParameterValueNormalizedById(const std::string& stableId, float value) override {
    const auto it = paramIdToIndex_.find(stableId);
    if (it == paramIdToIndex_.end()) {
      return false;
    }
    const int paramIndex = it->second;
    if (paramIndex < 0 || paramIndex >= paramTargetCount_ || !paramTargets_) {
      return false;
    }
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    paramTargets_[paramIndex].store(clamped, std::memory_order_relaxed);
    return true;
  }

  std::string getParameterTextById(const std::string& stableId,
                                   float normalized) const override {
    const auto it = paramIdToIndex_.find(stableId);
    if (!instance_ || it == paramIdToIndex_.end()) {
      return {};
    }
    const int paramIndex = it->second;
    if (paramIndex < 0 || paramIndex >= static_cast<int>(paramPointers_.size())) {
      return {};
    }
    const float clamped = std::clamp(normalized, 0.0f, 1.0f);
    return paramPointers_[paramIndex]->getText(clamped, 512).toStdString();
  }

  std::vector<uint8_t> getState() const override {
    if (!instance_) {
      return {};
    }
    juce::MemoryBlock data;
    instance_->getStateInformation(data);
    const auto* bytes = static_cast<const uint8_t*>(data.getData());
    return std::vector<uint8_t>(bytes, bytes + data.getSize());
  }

  bool setState(const std::vector<uint8_t>& data) override {
    if (!instance_ || data.empty()) {
      return false;
    }
    instance_->setStateInformation(data.data(), static_cast<int>(data.size()));
    return true;
  }

  void flushParameterChanges() override { applyPendingParameterChanges(); }

  bool openEditor() override {
    if (!instance_) {
      std::cerr << "JucePluginInstance: no instance for editor" << std::endl;
      return false;
    }
    auto* manager = juce::MessageManager::getInstance();
    if (!manager) {
      std::cerr << "JucePluginInstance: message manager unavailable" << std::endl;
      return false;
    }
    if (manager->isThisTheMessageThread()) {
      return openEditorOnMessageThread();
    }
    const bool queued = juce::MessageManager::callAsync([this]() {
      openEditorOnMessageThread();
    });
    if (!queued) {
      std::cerr << "JucePluginInstance: failed to queue editor open" << std::endl;
    }
    return queued;
  }

 private:
  bool openEditorOnMessageThread() {
    if (!instance_) {
      return false;
    }
#if JUCE_MAC
    juce::Process::setDockIconVisible(true);
    juce::Process::makeForegroundProcess();
#endif
    if (editorWindow_) {
      editorWindow_->setVisible(true);
      editorWindow_->toFront(true);
      return true;
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor(
        instance_->createEditorIfNeeded());
    if (!editor) {
      std::cerr << "JucePluginInstance: editor unavailable for "
                << instance_->getName().toStdString() << std::endl;
      return false;
    }
    struct EditorWindow final : public juce::DocumentWindow {
      explicit EditorWindow(const juce::String& name, JucePluginInstance* ownerIn)
          : juce::DocumentWindow(name, juce::Colours::black,
                                 juce::DocumentWindow::closeButton),
            owner(ownerIn) {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
      }
      void closeButtonPressed() override {
        if (owner) {
          owner->closeEditorWindow();
        }
      }
      JucePluginInstance* owner = nullptr;
    };
    editorWindow_ =
        std::make_unique<EditorWindow>(instance_->getName(), this);
    editorWindow_->setContentOwned(editor.release(), true);
    const int width = std::max(200, editorWindow_->getWidth());
    const int height = std::max(200, editorWindow_->getHeight());
    editorWindow_->setSize(width, height);
    editorWindow_->centreWithSize(width, height);
    editorWindow_->setVisible(true);
    editorWindow_->toFront(true);
    return true;
  }

  void closeEditorWindow() {
    if (!editorWindow_) {
      return;
    }
    editorWindow_->setVisible(false);
    editorWindow_.reset();
  }

  void buildParameterCache() {
    if (!instance_) {
      return;
    }

    const auto& parameters = instance_->getParameters();
    params_.clear();
    paramPointers_.clear();
    lastApplied_.clear();
    paramIdToIndex_.clear();
    paramTargetCount_ = 0;
    paramTargets_.reset();

    params_.reserve(parameters.size());
    paramPointers_.reserve(parameters.size());
    lastApplied_.reserve(parameters.size());

    for (int i = 0; i < parameters.size(); ++i) {
      auto* param = parameters.getUnchecked(i);
      paramPointers_.push_back(param);

      ParamInfo info;
      info.index = i;
      if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(param)) {
        info.stableId = withId->paramID.toStdString();
      } else {
        info.stableId = "index:" + std::to_string(i);
      }
      info.name = param->getName(512).toStdString();
      info.label = param->getLabel().toStdString();
      info.defaultNormalized = param->getDefaultValue();
      info.isDiscrete = param->isDiscrete();
      info.isAutomatable = param->isAutomatable();

      if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param)) {
        const auto& range = ranged->getNormalisableRange();
        info.minValue = range.start;
        info.maxValue = range.end;
      }

      paramIdToIndex_[info.stableId] = i;
      params_.push_back(std::move(info));
      const float current = param->getValue();
      lastApplied_.push_back(current);
    }

    paramTargetCount_ = static_cast<int>(paramPointers_.size());
    if (paramTargetCount_ > 0) {
      paramTargets_ = std::make_unique<std::atomic<float>[]>(paramTargetCount_);
      for (int i = 0; i < paramTargetCount_; ++i) {
        paramTargets_[i].store(lastApplied_[i], std::memory_order_relaxed);
      }
    }
  }

  void applyPendingParameterChanges() {
    if (paramTargetCount_ <= 0 || !paramTargets_) {
      return;
    }

    for (int i = 0; i < paramTargetCount_; ++i) {
      const float value = paramTargets_[i].load(std::memory_order_relaxed);
      if (value != lastApplied_[i]) {
        paramPointers_[i]->setValue(value);
        lastApplied_[i] = value;
      }
    }
  }

  bool processWithVst3Events(juce::AudioBuffer<float>& buffer,
                             const float* const* inputs,
                             int numInputs,
                             int numOutputs,
                             int numFrames,
                             const MidiEvents& events) {
    auto* component =
        static_cast<Steinberg::Vst::IComponent*>(instance_->getPlatformSpecificData());
    if (!component) {
      return false;
    }
    Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor> processor(component);
    if (processor == nullptr) {
      return false;
    }

    std::vector<float*> inputPtrs;
    if (inputs != nullptr && numInputs > 0) {
      inputPtrs.reserve(static_cast<size_t>(numInputs));
      for (int ch = 0; ch < numInputs; ++ch) {
        inputPtrs.push_back(const_cast<float*>(inputs[ch]));
      }
    }

    std::vector<float*> outputPtrs;
    outputPtrs.reserve(static_cast<size_t>(numOutputs));
    for (int ch = 0; ch < numOutputs; ++ch) {
      outputPtrs.push_back(buffer.getWritePointer(ch));
    }

    Steinberg::Vst::AudioBusBuffers inputBus{};
    Steinberg::Vst::AudioBusBuffers outputBus{};
    if (!inputPtrs.empty()) {
      inputBus.numChannels = static_cast<Steinberg::int32>(inputPtrs.size());
      inputBus.channelBuffers32 = inputPtrs.data();
    }
    outputBus.numChannels = static_cast<Steinberg::int32>(outputPtrs.size());
    outputBus.channelBuffers32 = outputPtrs.data();

    juce::MidiEventList inputEvents;
    juce::MidiEventList outputEvents;

    for (const auto& ev : events) {
      const uint8_t type = ev.status & 0xF0u;
      Steinberg::Vst::Event e{};
      e.busIndex = 0;
      e.sampleOffset = ev.sampleOffset;
      if (type == 0x90u) {
        e.type = Steinberg::Vst::Event::kNoteOnEvent;
        e.noteOn.channel = static_cast<Steinberg::int16>(ev.channel & 0x0F);
        e.noteOn.pitch = static_cast<Steinberg::int16>(ev.data1);
        e.noteOn.velocity = static_cast<float>(ev.data2) / 127.0f;
        e.noteOn.tuning = ev.tuningCents;
        e.noteOn.noteId = ev.noteId > 0 ? ev.noteId : -1;
        inputEvents.addEvent(e);
      } else if (type == 0x80u || (type == 0x90u && ev.data2 == 0)) {
        e.type = Steinberg::Vst::Event::kNoteOffEvent;
        e.noteOff.channel = static_cast<Steinberg::int16>(ev.channel & 0x0F);
        e.noteOff.pitch = static_cast<Steinberg::int16>(ev.data1);
        e.noteOff.velocity = static_cast<float>(ev.data2) / 127.0f;
        e.noteOff.tuning = ev.tuningCents;
        e.noteOff.noteId = ev.noteId > 0 ? ev.noteId : -1;
        inputEvents.addEvent(e);
      } else if (type == 0xA0u) {
        e.type = Steinberg::Vst::Event::kPolyPressureEvent;
        e.polyPressure.channel = static_cast<Steinberg::int16>(ev.channel & 0x0F);
        e.polyPressure.pitch = static_cast<Steinberg::int16>(ev.data1);
        e.polyPressure.pressure = static_cast<float>(ev.data2) / 127.0f;
        e.polyPressure.noteId = ev.noteId > 0 ? ev.noteId : -1;
        inputEvents.addEvent(e);
      }
    }

    Steinberg::Vst::ProcessData data{};
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = static_cast<Steinberg::int32>(numFrames);
    data.numInputs = inputPtrs.empty() ? 0 : 1;
    data.numOutputs = 1;
    data.inputs = inputPtrs.empty() ? nullptr : &inputBus;
    data.outputs = &outputBus;
    data.inputEvents = &inputEvents;
    data.outputEvents = &outputEvents;

    const auto result = processor->process(data);
    return result == Steinberg::kResultOk || result == Steinberg::kResultTrue;
  }

  std::unique_ptr<juce::AudioPluginInstance> instance_;
  EnginePlayHead playHead_;
  std::string vendor_;
  std::string identifier_;
  std::string version_;
  std::array<uint8_t, 16> uid16_;
  int pluginOutputs_ = 0;
  std::vector<BusInfo> busLayout_;
  juce::AudioBuffer<float> scratch_;
  std::vector<ParamInfo> params_;
  std::vector<juce::AudioProcessorParameter*> paramPointers_;
  std::vector<float> lastApplied_;
  std::unique_ptr<std::atomic<float>[]> paramTargets_;
  int paramTargetCount_ = 0;
  std::unordered_map<std::string, int> paramIdToIndex_;
  std::unique_ptr<juce::DocumentWindow> editorWindow_;
};

class FakeIdentityPluginInstance final : public IPluginInstance {
 public:
  // latencySamples models a real plugin's reported processing latency: the instance
  // both REPORTS it (getLatencySamples -> the GetLatency wire) and APPLIES it (delays
  // its output by that many samples, as an internal look-ahead/oversampling plugin
  // would). That makes it the fixture for an end-to-end PDC null test — a latent track
  // and a dry one land aligned only if compensation actually delayed the dry one.
  explicit FakeIdentityPluginInstance(int latencySamples = 0)
      : latencySamples_(std::max(0, latencySamples)) {
    ParamInfo gainInfo;
    gainInfo.stableId = "index:0";
    gainInfo.index = 0;
    gainInfo.name = "Gain";
    gainInfo.defaultNormalized = 1.0f;
    gainInfo.minValue = 0.0f;
    gainInfo.maxValue = 1.0f;
    gainInfo.isAutomatable = true;
    params_.push_back(std::move(gainInfo));
  }

  void prepare(double, int blockSize, int numOutputs,
               bool enableSidechain = false, bool enableAuxOut = false) override {
    outputChannels_ = numOutputs;
    sidechainEnabled_ = enableSidechain;
    auxOutEnabled_ = enableAuxOut;
    // Size the latency delay ring to hold one full latency window plus a block, so a
    // pulse generated this block reads out latencySamples_ later without wrapping into
    // itself. Zero-filled: the first latencySamples_ of output are silence, exactly as
    // a real latent plugin ramps up.
    if (latencySamples_ > 0) {
      delayLen_ = static_cast<uint32_t>(latencySamples_ + std::max(1, blockSize) + 1);
      delayRing_.assign(delayLen_, 0.0f);
      delayWrite_ = 0;
    }
  }

  // The identity plugin has no time-dependent behaviour.
  void setTransport(const TransportInfo&) override {}

  void process(const float* const* inputs,
               int numInputs,
               float* const* outputs,
               int numOutputs,
               int numFrames,
               const MidiEvents& events,
               int64_t,
               float* const* auxOutputs = nullptr,
               int numAuxOutputs = 0) override {
    for (int ch = 0; ch < numOutputs; ++ch) {
      std::fill(outputs[ch], outputs[ch] + numFrames, 0.0f);
    }
    for (int ch = 0; ch < numAuxOutputs; ++ch) {
      if (auxOutputs && auxOutputs[ch]) {
        std::fill(auxOutputs[ch], auxOutputs[ch] + numFrames, 0.0f);
      }
    }

    // Multi-out fixture: with aux outputs enabled, route each note to output bus
    // (pitch % numBuses) — bus 0 is the main output, bus k>0 lands on aux bus k-1. So a
    // note at pitch 60 goes to main, 61 to aux bus 0, 62 to aux bus 1, letting a test
    // prove each stem reaches its own child track. Pulses are 10 samples, like the
    // single-bus path.
    if (auxOutEnabled_) {
      const int numBuses = 1 + kFakeAuxOutBuses;
      const float gain = gain_;
      for (const auto& event : events) {
        const uint8_t status = event.status & 0xF0u;
        if (status != 0x90u || event.data2 == 0) {
          continue;
        }
        const int bus = event.data1 % numBuses;
        const int start = std::max(0, event.sampleOffset);
        const int end = std::min(numFrames, start + 10);
        if (bus == 0) {
          for (int ch = 0; ch < numOutputs; ++ch) {
            for (int i = start; i < end; ++i) outputs[ch][i] += gain;
          }
        } else {
          for (int ch = 0; ch < kFakeAuxBusChannels; ++ch) {
            const int idx = (bus - 1) * kFakeAuxBusChannels + ch;
            if (auxOutputs && idx < numAuxOutputs && auxOutputs[idx]) {
              for (int i = start; i < end; ++i) auxOutputs[idx][i] += gain;
            }
          }
        }
      }
      return;
    }

    // Sidechain fixture: with the key input enabled, pass the SIDECHAIN bus straight to
    // the output (ignoring MIDI). The sidechain occupies the input channels after the
    // main bus — the host lays out [main..., sidechain...] — so a null test proves the
    // key signal actually reached the plugin: the sidechained track's output equals the
    // source track's output, sample for sample.
    if (sidechainEnabled_) {
      const int mainCh = kFakeMainInputChannels;
      for (int ch = 0; ch < numOutputs; ++ch) {
        const int scIndex = mainCh + ch;
        if (inputs && scIndex < numInputs && inputs[scIndex]) {
          std::copy(inputs[scIndex], inputs[scIndex] + numFrames, outputs[ch]);
        }
      }
      return;
    }

    // Build this block's dry mono output from note-ons (a 10-sample pulse each), then
    // either write it straight to every channel or push it through the latency ring.
    const float gain = gain_;
    dryScratch_.assign(numFrames, 0.0f);
    for (const auto& event : events) {
      const uint8_t status = event.status & 0xF0u;
      if (status != 0x90u || event.data2 == 0) {
        continue;
      }
      const int start = std::max(0, event.sampleOffset);
      const int end = std::min(numFrames, start + 10);
      for (int i = start; i < end; ++i) {
        dryScratch_[i] += gain;
      }
    }

    if (latencySamples_ <= 0 || delayLen_ == 0) {
      for (int ch = 0; ch < numOutputs; ++ch) {
        for (int i = 0; i < numFrames; ++i) {
          outputs[ch][i] += dryScratch_[i];
        }
      }
      return;
    }

    // Latent path: delay the dry signal by latencySamples_ through the ring, then fan
    // the delayed sample out to every channel. Models a plugin whose output trails its
    // input — the offset PDC exists to remove.
    uint32_t w = delayWrite_;
    const uint32_t lat = static_cast<uint32_t>(latencySamples_);
    for (int i = 0; i < numFrames; ++i) {
      delayRing_[w] = dryScratch_[i];
      const uint32_t r = (w + delayLen_ - lat) % delayLen_;
      const float delayed = delayRing_[r];
      w = (w + 1 == delayLen_) ? 0 : w + 1;
      for (int ch = 0; ch < numOutputs; ++ch) {
        outputs[ch][i] += delayed;
      }
    }
    delayWrite_ = w;
  }

  std::string name() const override { return "Identity"; }
  std::string vendor() const override { return "Unidaw"; }
  std::string identifier() const override { return "identity.fake"; }
  std::string version() const override { return "1.0"; }
  std::array<uint8_t, 16> pluginUid16() const override { return {}; }
  int numParameters() const override { return static_cast<int>(params_.size()); }
  // With the sidechain enabled the fixture declares a main input bus plus a sidechain
  // input bus, so the host lays out [main..., sidechain...] and process() can read the
  // key from the channels after the main bus.
  int inputChannels() const override {
    return sidechainEnabled_ ? (kFakeMainInputChannels + kFakeSidechainChannels) : 0;
  }
  int outputChannels() const override {
    return auxOutEnabled_
               ? (outputChannels_ + kFakeAuxOutBuses * kFakeAuxBusChannels)
               : outputChannels_;
  }
  int latencySamples() const override { return latencySamples_; }
  std::vector<BusInfo> busLayout() const override {
    std::vector<BusInfo> buses;
    if (sidechainEnabled_) {
      BusInfo mainIn;
      mainIn.isInput = true;
      mainIn.index = 0;
      mainIn.isMain = true;
      mainIn.enabled = true;
      mainIn.channelCount = kFakeMainInputChannels;
      mainIn.channelOffset = 0;
      mainIn.layoutId = 2;
      mainIn.name = "Main";
      mainIn.layout = "Stereo";
      buses.push_back(mainIn);
      BusInfo sc;
      sc.isInput = true;
      sc.index = 1;
      sc.isMain = false;
      sc.enabled = true;
      sc.channelCount = kFakeSidechainChannels;
      sc.channelOffset = kFakeMainInputChannels;
      sc.layoutId = 2;
      sc.name = "Sidechain";
      sc.layout = "Stereo";
      buses.push_back(sc);
    }
    BusInfo out;
    out.isInput = false;
    out.index = 0;
    out.isMain = true;
    out.enabled = true;
    out.channelCount = outputChannels_;
    out.channelOffset = 0;
    out.layoutId = outputChannels_ == 1 ? 1 : 2;
    out.name = "Main";
    out.layout = outputChannels_ == 1 ? "Mono" : "Stereo";
    buses.push_back(out);
    // Multi-out fixture: kFakeAuxOutBuses stereo aux output buses (the "stems"), each
    // offset by its channels — the shape a drum plugin's kick/snare/hat outs take.
    if (auxOutEnabled_) {
      int offset = outputChannels_;
      for (int b = 0; b < kFakeAuxOutBuses; ++b) {
        BusInfo aux;
        aux.isInput = false;
        aux.index = b + 1;
        aux.isMain = false;
        aux.enabled = true;
        aux.channelCount = kFakeAuxBusChannels;
        aux.channelOffset = offset;
        aux.layoutId = 2;
        aux.name = "Stem " + std::to_string(b + 1);
        aux.layout = "Stereo";
        buses.push_back(aux);
        offset += kFakeAuxBusChannels;
      }
    }
    return buses;
  }
  bool loadVst3PresetFile(const std::string&) override { return false; }

  const std::vector<ParamInfo>& parameters() const override { return params_; }

  float getParameterValueNormalizedById(const std::string& stableId) const override {
    if (stableId == params_[0].stableId) {
      return gain_;
    }
    return 0.0f;
  }

  bool setParameterValueNormalizedById(const std::string& stableId, float value) override {
    if (stableId == params_[0].stableId) {
      gain_ = std::clamp(value, 0.0f, 1.0f);
      return true;
    }
    return false;
  }

  std::string getParameterTextById(const std::string& stableId,
                                   float normalized) const override {
    if (stableId == params_[0].stableId) {
      return std::to_string(normalized);
    }
    return {};
  }

  std::vector<uint8_t> getState() const override {
    std::vector<uint8_t> data(sizeof(float), 0);
    std::memcpy(data.data(), &gain_, sizeof(float));
    return data;
  }

  bool setState(const std::vector<uint8_t>& data) override {
    if (data.size() != sizeof(float)) {
      return false;
    }
    std::memcpy(&gain_, data.data(), sizeof(float));
    gain_ = std::clamp(gain_, 0.0f, 1.0f);
    return true;
  }

  void flushParameterChanges() override {}
  bool openEditor() override { return false; }

 private:
  static constexpr int kFakeMainInputChannels = 2;
  static constexpr int kFakeSidechainChannels = 2;
  static constexpr int kFakeAuxOutBuses = 2;      // stereo stems the fixture emits
  static constexpr int kFakeAuxBusChannels = 2;
  std::vector<ParamInfo> params_;
  float gain_ = 1.0f;
  int outputChannels_ = 2;
  // Test-latency simulation (see the constructor comment). 0 = a plain identity plugin.
  int latencySamples_ = 0;
  std::vector<float> delayRing_;
  std::vector<float> dryScratch_;
  uint32_t delayLen_ = 0;
  uint32_t delayWrite_ = 0;
  // Sidechain fixture: when true, process() passes the sidechain input to the output.
  bool sidechainEnabled_ = false;
  // Multi-out fixture: when true, notes route to output bus (pitch % (1+auxBuses)).
  bool auxOutEnabled_ = false;
};

class JucePluginHost final : public IPluginHost {
 public:
  JucePluginHost() {
    formatManager_.addFormat(std::make_unique<juce::VST3PluginFormat>());
  }

  std::unique_ptr<IPluginInstance> loadVst3FromPath(const std::string& path,
                                                    const std::string& desiredName,
                                                    double sampleRate,
                                                    int blockSize) override {
    const bool logLoad = std::getenv("DAW_HOST_LOG_LOAD") != nullptr;
    if (const char* env = std::getenv("DAW_USE_FAKE_IDENTITY")) {
      if (std::string(env) == "1") {
        const auto filename = juce::File(path).getFileName();
        if (filename == "Identity.vst3") {
          // A "latency:N" desiredName gives the fake N samples of reported+applied
          // processing latency, for the PDC null test. Any other name = plain identity.
          int fakeLatency = 0;
          const std::string prefix = "latency:";
          if (desiredName.rfind(prefix, 0) == 0) {
            fakeLatency = std::atoi(desiredName.c_str() + prefix.size());
          }
          return std::make_unique<FakeIdentityPluginInstance>(fakeLatency);
        }
      }
    }

    juce::String error;
    juce::OwnedArray<juce::PluginDescription> types;

    for (int i = 0; i < formatManager_.getNumFormats(); ++i) {
      auto* format = formatManager_.getFormat(i);
      if (logLoad) {
        std::cerr << "Host: scanning VST3 types for " << path << std::endl;
      }
      format->findAllTypesForFile(types, path);
    }
    if (logLoad) {
      std::cerr << "Host: VST3 scan found " << types.size() << " type(s)" << std::endl;
    }

    if (types.isEmpty()) {
      std::cerr << "No VST3 types found for: " << path << std::endl;
      return nullptr;
    }

    // A VST3 bundle can hold several plugins (u-he Zebra2.vst3 ships Zebra2 +
    // Zebralette + more; NI/Waves bundles hold dozens). `desiredName` says which one
    // the project meant; select it by name so we don't silently load the wrong
    // sub-plugin — exactly the "loaded the wrong plugin" failure the resolver exists
    // to prevent. Empty name, or no match, falls back to the first type.
    const auto* description = types.getFirst();
    if (!desiredName.empty()) {
      const juce::String want(desiredName);
      const juce::PluginDescription* match = nullptr;
      for (const auto* d : types) {
        if (d->name == want) {
          match = d;
          break;
        }
      }
      if (match) {
        description = match;
      } else if (types.size() > 1) {
        juce::String names;
        for (int i = 0; i < types.size(); ++i) {
          names << (i ? ", " : "") << types[i]->name;
        }
        std::cerr << "Host: bundle " << path << " has no plugin named '"
                  << want << "' among [" << names << "] — loading first ("
                  << description->name << ")" << std::endl;
      }
    } else if (types.size() > 1) {
      juce::String names;
      for (int i = 0; i < types.size(); ++i) {
        names << (i ? ", " : "") << types[i]->name;
      }
      std::cerr << "Host: multi-plugin VST3 bundle " << path << " has "
                << types.size() << " types [" << names
                << "] and no requested name — loading first ("
                << description->name << ")" << std::endl;
    }
    std::unique_ptr<juce::AudioPluginInstance> instance;
    if (logLoad) {
      std::cerr << "Host: creating VST3 instance for " << path << std::endl;
    }
    // Create instance directly - avoid callAsync+wait pattern which can deadlock
    // when called from the control thread while message thread is busy.
    instance = formatManager_.createPluginInstance(*description,
                                                   sampleRate,
                                                   blockSize,
                                                   error);
    if (instance == nullptr) {
      std::cerr << "Failed to create plugin instance: " << error << std::endl;
#if JUCE_MAC
      std::string slowError;
      auto slowDescriptions =
          buildVst3DescriptionsFromFactory(juce::File(path), &slowError);
      // Try the requested sub-plugin first, so a multi-plugin bundle still lands on
      // the right one on the slow path (not just whichever instantiates first).
      if (!desiredName.empty()) {
        const juce::String want(desiredName);
        std::stable_sort(slowDescriptions.begin(), slowDescriptions.end(),
                         [&](const juce::PluginDescription& a,
                             const juce::PluginDescription& b) {
                           return (a.name == want) && (b.name != want);
                         });
      }
      for (const auto& desc : slowDescriptions) {
        juce::String slowErr;
        auto slowInstance = formatManager_.createPluginInstance(desc, sampleRate, blockSize,
                                                                slowErr);
        if (slowInstance != nullptr) {
          return std::make_unique<JucePluginInstance>(
              std::move(slowInstance),
              desc.manufacturerName.toStdString(),
              desc.createIdentifierString().toStdString(),
              desc.version.toStdString());
        }
      }
      if (!slowError.empty()) {
        std::cerr << "VST3 slow path error: " << slowError << std::endl;
      }
      logVst3BundleLoadFailure(juce::File(path));
#endif
      return nullptr;
    }

    return std::make_unique<JucePluginInstance>(
        std::move(instance),
        description->manufacturerName.toStdString(),
        description->createIdentifierString().toStdString(),
        description->version.toStdString());
  }

 private:
  juce::AudioPluginFormatManager formatManager_;
};

}  // namespace

std::unique_ptr<IRuntime> createJuceRuntime() {
  return std::make_unique<JuceRuntime>();
}

std::unique_ptr<IAudioBackend> createAudioBackend() {
  return std::make_unique<JuceAudioBackend>();
}

std::unique_ptr<IPluginHost> createPluginHost() {
  return std::make_unique<JucePluginHost>();
}

namespace {

int64_t nowUnixMs() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
  return ms.count();
}

bool isVst3Path(const juce::File& file) {
  return file.hasFileExtension(".vst3");
}

std::vector<juce::File> collectVst3Paths(const std::vector<std::string>& paths) {
  std::vector<juce::File> results;
  for (const auto& path : paths) {
    juce::File file(path);
    if (file.exists()) {
      if (file.isDirectory()) {
        juce::Array<juce::File> children;
        file.findChildFiles(children, juce::File::findFilesAndDirectories, true, "*.vst3");
        for (const auto& child : children) {
          if (isVst3Path(child)) {
            results.push_back(child);
          }
        }
      } else if (isVst3Path(file)) {
        results.push_back(file);
      }
    }
  }
  return results;
}

}  // namespace

std::vector<std::string> discoverVst3Candidates(const std::vector<std::string>& paths) {
  std::vector<std::string> results;
  const auto files = collectVst3Paths(paths);
  results.reserve(files.size());
  for (const auto& file : files) {
    results.push_back(file.getFullPathName().toStdString());
  }
  return results;
}

std::vector<PluginScanResult> scanVst3File(const std::string& path,
                                           bool instantiate,
                                           double sampleRate,
                                           int blockSize) {
  juce::AudioPluginFormatManager formatManager;
  formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());

  std::vector<PluginScanResult> results;
  const auto scanTime = nowUnixMs();

  juce::OwnedArray<juce::PluginDescription> types;
  for (int i = 0; i < formatManager.getNumFormats(); ++i) {
    auto* format = formatManager.getFormat(i);
    format->findAllTypesForFile(types, path);
  }

  if (types.isEmpty()) {
    PluginScanResult result;
    result.path = path;
    result.format = "VST3";
    result.ok = false;
    result.error = "No plugin types found";
    result.scanTimeUnixMs = scanTime;
    results.push_back(std::move(result));
    return results;
  }

  for (const auto* desc : types) {
    PluginScanResult result;
    result.path = path;
    result.identifier = desc->createIdentifierString().toStdString();
    result.uid16Hex = md5UidHexFromIdentifier(result.identifier);
    result.uid = desc->createIdentifierString().toStdString();
    result.name = desc->name.toStdString();
    result.vendor = desc->manufacturerName.toStdString();
    result.version = desc->version.toStdString();
    result.format = desc->pluginFormatName.toStdString();
    result.category = desc->category.toStdString();
    result.isInstrument = desc->isInstrument;
    result.numInputChannels = desc->numInputChannels;
    result.numOutputChannels = desc->numOutputChannels;
    result.ok = true;
    result.scanTimeUnixMs = scanTime;

    if (instantiate) {
      juce::String error;
      auto instance = formatManager.createPluginInstance(*desc, sampleRate, blockSize, error);
      if (instance != nullptr) {
        result.paramCount = instance->getParameters().size();
        result.hasEditor = instance->hasEditor();
      } else {
        result.ok = false;
        result.error = error.toStdString();
        result.hasEditor = false;
#if JUCE_MAC
        std::string slowError;
        const auto slowDescriptions =
            buildVst3DescriptionsFromFactory(juce::File(path), &slowError);
        for (const auto& slowDesc : slowDescriptions) {
          juce::String slowErr;
          auto slowInstance =
              formatManager.createPluginInstance(slowDesc, sampleRate, blockSize, slowErr);
          if (slowInstance != nullptr) {
            result.ok = true;
            result.error.clear();
            result.paramCount = slowInstance->getParameters().size();
            result.hasEditor = slowInstance->hasEditor();
            break;
          }
        }
        if (!result.ok && !slowError.empty()) {
          result.error += " (slow path: " + slowError + ")";
        }
        logVst3BundleLoadFailure(juce::File(path));
#endif
      }
    }

    results.push_back(std::move(result));
  }

  return results;
}

DecodedAudio decodeAudioFileMono(const std::string& path) {
  DecodedAudio out;
  juce::AudioFormatManager formats;
  formats.registerBasicFormats();
  std::unique_ptr<juce::AudioFormatReader> reader(
      formats.createReaderFor(juce::File(juce::String(path))));
  if (!reader) {
    return out;
  }
  const int channels = static_cast<int>(reader->numChannels);
  const juce::int64 frames = reader->lengthInSamples;
  if (channels <= 0 || frames <= 0) {
    return out;
  }
  juce::AudioBuffer<float> buffer(channels, static_cast<int>(frames));
  buffer.clear();
  reader->read(&buffer, 0, static_cast<int>(frames), 0, true, true);

  out.frames = static_cast<uint64_t>(frames);
  out.sampleRate = reader->sampleRate;
  out.sourceChannels = static_cast<uint32_t>(channels);

  // Build the waveform pyramid from the multi-channel buffer NOW, before the
  // downmix below throws the per-channel data away. getReadPointer gives the
  // planar float channel this reads.
  {
    std::vector<const float*> chanPtrs(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) chanPtrs[c] = buffer.getReadPointer(c);
    out.pyramid = std::make_shared<const WaveformPyramid>(buildWaveformPyramid(
        chanPtrs.data(), static_cast<uint32_t>(channels),
        static_cast<uint64_t>(frames)));
  }

  out.samples.resize(static_cast<size_t>(frames));
  const float invChannels = 1.0f / static_cast<float>(channels);
  for (juce::int64 i = 0; i < frames; ++i) {
    float sum = 0.0f;
    for (int c = 0; c < channels; ++c) {
      sum += buffer.getSample(c, static_cast<int>(i));
    }
    out.samples[static_cast<size_t>(i)] = sum * invChannels;
  }
  out.ok = true;
  return out;
}

bool writeWavMono(const std::string& path, const float* samples, uint64_t frames,
                  double sampleRate) {
  if (!samples || sampleRate <= 0.0) {
    return false;
  }
  juce::File file{juce::String(path)};
  file.deleteFile();
  std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
  if (!stream) {
    return false;
  }
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatWriter> writer(
      wav.createWriterFor(stream.get(), sampleRate, 1, 16, {}, 0));
  if (!writer) {
    return false;
  }
  stream.release();  // the writer owns the stream now
  juce::AudioBuffer<float> buffer(1, static_cast<int>(frames));
  for (uint64_t i = 0; i < frames; ++i) {
    buffer.setSample(0, static_cast<int>(i), samples[i]);
  }
  const bool ok =
      writer->writeFromAudioSampleBuffer(buffer, 0, static_cast<int>(frames));
  writer.reset();  // flush + close
  return ok;
}

}  // namespace daw
