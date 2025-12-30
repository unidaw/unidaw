#include "platform_juce/juce_wrapper.h"

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

  void prepare(double sampleRate, int blockSize, int numOutputs) override {
    if (!instance_) {
      return;
    }

    instance_->setNonRealtime(false);
    juce::AudioProcessor::BusesLayout layout;
    if (numOutputs == 1) {
      layout.outputBuses.add(juce::AudioChannelSet::mono());
      layout.inputBuses.add(juce::AudioChannelSet::mono());
    } else if (numOutputs >= 2) {
      layout.outputBuses.add(juce::AudioChannelSet::stereo());
      layout.inputBuses.add(juce::AudioChannelSet::stereo());
    }
    instance_->enableAllBuses();
    instance_->setBusesLayout(layout);
    instance_->setPlayConfigDetails(0, numOutputs, sampleRate, blockSize);
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
               const MidiEvents& events, int64_t samplePosition) override {
    if (!instance_ || outputs == nullptr || numOutputs <= 0 || numFrames <= 0) {
      return;
    }

    applyPendingParameterChanges();
    juce::AudioBuffer<float> buffer(outputs, numOutputs, numFrames);
    juce::AudioBuffer<float>* bufferToProcess = &buffer;

    if (inputs != nullptr && numInputs > 0) {
      const int channelsToCopy = std::min(numInputs, numOutputs);
      for (int ch = 0; ch < channelsToCopy; ++ch) {
        const float* src = inputs[ch];
        float* dest = buffer.getWritePointer(ch);
        std::copy(src, src + numFrames, dest);
      }
      for (int ch = channelsToCopy; ch < numOutputs; ++ch) {
        std::fill(buffer.getWritePointer(ch), buffer.getWritePointer(ch) + numFrames, 0.0f);
      }
    }

    if (pluginOutputs_ > 0 && pluginOutputs_ != numOutputs) {
      scratch_.setSize(pluginOutputs_, numFrames, false, false, true);
      scratch_.clear();
      bufferToProcess = &scratch_;
    } else {
      if (inputs == nullptr || numInputs == 0) {
        buffer.clear();
      }
    }

    if (!processWithVst3Events(*bufferToProcess, inputs, numInputs,
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

  int outputChannels() const override { return pluginOutputs_; }

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

 private:
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
  std::string vendor_;
  std::string identifier_;
  std::string version_;
  std::array<uint8_t, 16> uid16_;
  int pluginOutputs_ = 0;
  juce::AudioBuffer<float> scratch_;
  std::vector<ParamInfo> params_;
  std::vector<juce::AudioProcessorParameter*> paramPointers_;
  std::vector<float> lastApplied_;
  std::unique_ptr<std::atomic<float>[]> paramTargets_;
  int paramTargetCount_ = 0;
  std::unordered_map<std::string, int> paramIdToIndex_;
};

class JucePluginHost final : public IPluginHost {
 public:
  JucePluginHost() {
    formatManager_.addFormat(std::make_unique<juce::VST3PluginFormat>());
  }

  std::unique_ptr<IPluginInstance> loadVst3FromPath(const std::string& path,
                                                    double sampleRate,
                                                    int blockSize) override {
    juce::String error;
    juce::OwnedArray<juce::PluginDescription> types;

    for (int i = 0; i < formatManager_.getNumFormats(); ++i) {
      auto* format = formatManager_.getFormat(i);
      format->findAllTypesForFile(types, path);
    }

    if (types.isEmpty()) {
      std::cerr << "No VST3 types found for: " << path << std::endl;
      return nullptr;
    }

    const auto* description = types.getFirst();
    auto instance = formatManager_.createPluginInstance(*description, sampleRate, blockSize,
                                                        error);
    if (instance == nullptr) {
      std::cerr << "Failed to create plugin instance: " << error << std::endl;
#if JUCE_MAC
      std::string slowError;
      const auto slowDescriptions =
          buildVst3DescriptionsFromFactory(juce::File(path), &slowError);
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

}  // namespace daw
