#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <array>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <algorithm>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <juce_events/juce_events.h>

#include "apps/audio_shm.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/ipc_protocol.h"
#include "apps/ipc_io.h"
#include "apps/shared_memory.h"
#include "platform_juce/juce_wrapper.h"
#include "apps/uid_hash.h"

namespace {

struct HostState {
  int serverFd = -1;
  int clientFd = -1;
  int shmFd = -1;
  void* shmBase = nullptr;
  size_t shmSize = 0;
  std::string shmName;
  daw::ShmHeader* header = nullptr;
  daw::BlockMailbox* mailbox = nullptr;
  daw::EventRingView ringStd;
  daw::EventRingView ringCtrl;
  std::unique_ptr<daw::IRuntime> runtime;
  std::unique_ptr<daw::IPluginHost> pluginHost;
  struct PluginSlot {
    std::unique_ptr<daw::IPluginInstance> instance;
    std::unordered_map<std::string, std::string> paramIdByUid16;
    bool bypass = false;
  };
  std::vector<PluginSlot> plugins;
  std::mutex pluginsMutex;
  std::vector<float*> inputPtrs;
  std::vector<float*> outputPtrs;
  std::vector<std::string> pluginPaths;
  std::vector<float> chainBufferA;
  std::vector<float> chainBufferB;
  std::vector<float*> chainPtrsA;
  std::vector<float*> chainPtrsB;
  bool testMode = false;
  std::atomic<bool> pluginsLoading{false};
  std::atomic<bool> pluginsReady{false};
};

std::string uid16Key(const uint8_t* uid16) {
  return std::string(reinterpret_cast<const char*>(uid16), 16);
}

int midiPriority(uint8_t status, uint8_t velocity) {
  const uint8_t type = status & 0xF0u;
  if (type == 0x80u || (type == 0x90u && velocity == 0)) {
    return 2;  // Note-off
  }
  if (type == 0x90u) {
    return 3;  // Note-on
  }
  return 3;
}

int eventPriority(const daw::EventEntry& entry) {
  if (entry.type == static_cast<uint16_t>(daw::EventType::Transport)) {
    return 0;
  }
  if (entry.type == static_cast<uint16_t>(daw::EventType::Param)) {
    return 1;
  }
  if (entry.type == static_cast<uint16_t>(daw::EventType::ReplayComplete)) {
    return 2;
  }
  if (entry.type == static_cast<uint16_t>(daw::EventType::Midi) &&
      entry.size >= sizeof(daw::MidiPayload)) {
    daw::MidiPayload payload{};
    std::memcpy(&payload, entry.payload, sizeof(payload));
    return midiPriority(payload.status, payload.data2);
  }
  return 3;
}

void closeFd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

std::string makeShmName() {
  if (const char* envName = std::getenv("DAW_SHM_NAME")) {
    std::string name(envName);
    if (!name.empty() && name.front() != '/') {
      name.insert(name.begin(), '/');
    }
    return name;
  }
  return "/daw_engine_shared";
}

int createServerSocket(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  ::unlink(path.c_str());
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closeFd(fd);
    return -1;
  }

  if (::listen(fd, 1) != 0) {
    closeFd(fd);
    return -1;
  }

  return fd;
}

void loadPlugins(HostState& state,
                 const std::vector<std::string>& pluginPaths,
                 double sampleRate,
                 uint32_t blockSize,
                 uint32_t numChannelsOut) {
  state.pluginsLoading.store(true, std::memory_order_release);
  state.pluginsReady.store(false, std::memory_order_release);
  const bool logLoad = std::getenv("DAW_HOST_LOG_LOAD") != nullptr;
  if (logLoad) {
    std::cerr << "Host: begin load (" << pluginPaths.size()
              << " paths)" << std::endl;
  }
  auto pluginHost = daw::createPluginHost();
  std::vector<HostState::PluginSlot> plugins;

  for (const auto& path : pluginPaths) {
    if (!std::filesystem::exists(path)) {
      std::cerr << "Plugin path not found: " << path << std::endl;
      continue;
    }
    if (logLoad) {
      std::cerr << "Host: loading plugin " << path << std::endl;
    }
    auto instance = pluginHost->loadVst3FromPath(path, sampleRate, blockSize);
    if (!instance) {
      std::cerr << "Failed to load plugin in host process: " << path << std::endl;
      continue;
    }
    if (logLoad) {
      std::cerr << "Host: loaded instance " << instance->name() << std::endl;
    }
    instance->prepare(sampleRate, blockSize,
                      static_cast<int>(numChannelsOut));
    HostState::PluginSlot slot;
    slot.instance = std::move(instance);
    for (const auto& param : slot.instance->parameters()) {
      const auto uid16 = daw::hashStableId16(param.stableId);
      slot.paramIdByUid16.emplace(uid16Key(uid16.data()), param.stableId);
    }
    plugins.push_back(std::move(slot));
  }

  std::vector<float> chainBufferA;
  std::vector<float> chainBufferB;
  std::vector<float*> chainPtrsA;
  std::vector<float*> chainPtrsB;
  if (plugins.size() > 1) {
    const size_t bufferSamples =
        static_cast<size_t>(numChannelsOut) * blockSize;
    chainBufferA.assign(bufferSamples, 0.0f);
    chainBufferB.assign(bufferSamples, 0.0f);
    chainPtrsA.resize(numChannelsOut);
    chainPtrsB.resize(numChannelsOut);
    for (uint32_t ch = 0; ch < numChannelsOut; ++ch) {
      chainPtrsA[ch] =
          chainBufferA.data() + static_cast<size_t>(ch) * blockSize;
      chainPtrsB[ch] =
          chainBufferB.data() + static_cast<size_t>(ch) * blockSize;
    }
  }

  std::lock_guard<std::mutex> lock(state.pluginsMutex);
  state.pluginHost = std::move(pluginHost);
  state.plugins = std::move(plugins);
  state.chainBufferA = std::move(chainBufferA);
  state.chainBufferB = std::move(chainBufferB);
  state.chainPtrsA = std::move(chainPtrsA);
  state.chainPtrsB = std::move(chainPtrsB);
  std::cerr << "Host: loaded " << state.plugins.size()
            << " plugin(s) from " << pluginPaths.size()
            << " path(s)" << std::endl;
  state.pluginsReady.store(true, std::memory_order_release);
  state.pluginsLoading.store(false, std::memory_order_release);
}

bool handleHello(HostState& state, const daw::HelloRequest& request) {
  state.shmName = makeShmName();
  const int shmFlags = O_CREAT | O_EXCL | O_RDWR;
  state.shmFd = ::shm_open(state.shmName.c_str(), shmFlags, 0600);
  if (state.shmFd < 0 && errno == EEXIST) {
    ::shm_unlink(state.shmName.c_str());
    state.shmFd = ::shm_open(state.shmName.c_str(), shmFlags, 0600);
  }
  if (state.shmFd < 0) {
    std::cerr << "shm_open failed for " << state.shmName
              << ": " << std::strerror(errno) << std::endl;
    return false;
  }
  std::cout << "SHM name: " << state.shmName << std::endl;

  daw::ShmHeader header;
  header.blockSize = request.blockSize;
  header.sampleRate = request.sampleRate;
  header.numChannelsIn = request.numChannelsIn;
  header.numChannelsOut = request.numChannelsOut;
  header.numBlocks = request.numBlocks;
  header.channelStrideBytes =
      static_cast<uint32_t>(daw::channelStrideBytes(request.blockSize));

  const size_t shmSize = daw::sharedMemorySize(header,
                                               request.ringStdCapacity,
                                               request.ringCtrlCapacity,
                                               request.ringUiCapacity);
  if (::ftruncate(state.shmFd, static_cast<off_t>(shmSize)) != 0) {
    std::cerr << "ftruncate failed: " << std::strerror(errno) << std::endl;
    return false;
  }

  state.shmBase = ::mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                         state.shmFd, 0);
  if (state.shmBase == MAP_FAILED) {
    state.shmBase = nullptr;
    std::cerr << "mmap failed: " << std::strerror(errno) << std::endl;
    return false;
  }

  state.shmSize = shmSize;
  state.header = reinterpret_cast<daw::ShmHeader*>(state.shmBase);

  size_t offset = daw::alignUp(sizeof(daw::ShmHeader), 64);
  const size_t stride = header.channelStrideBytes;
  const size_t inBlockBytes =
      static_cast<size_t>(header.numChannelsIn) * stride * header.numBlocks;
  const size_t outBlockBytes =
      static_cast<size_t>(header.numChannelsOut) * stride * header.numBlocks;

  header.audioInOffset = offset;
  offset += daw::alignUp(inBlockBytes, 64);
  header.audioOutOffset = offset;
  offset += daw::alignUp(outBlockBytes, 64);
  header.ringStdOffset = offset;
  offset += daw::alignUp(daw::ringBytes(request.ringStdCapacity), 64);
  header.ringCtrlOffset = offset;
  offset += daw::alignUp(daw::ringBytes(request.ringCtrlCapacity), 64);
  header.ringUiOffset = offset;
  offset += daw::alignUp(daw::ringBytes(request.ringUiCapacity), 64);
  header.mailboxOffset = offset;

  std::memcpy(state.header, &header, sizeof(header));
  state.header->uiVisualSampleCount = 0;
  state.header->uiGlobalNanotickPlayhead = 0;
  state.header->uiTrackCount = 0;
  for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
    state.header->uiTrackPeakRms[i] = 0.0f;
  }

  auto* ringStd = reinterpret_cast<daw::RingHeader*>(
      reinterpret_cast<uint8_t*>(state.shmBase) + header.ringStdOffset);
  ringStd->capacity = request.ringStdCapacity;
  ringStd->entrySize = sizeof(daw::EventEntry);
  ringStd->readIndex.store(0);
  ringStd->writeIndex.store(0);

  auto* ringCtrl = reinterpret_cast<daw::RingHeader*>(
      reinterpret_cast<uint8_t*>(state.shmBase) + header.ringCtrlOffset);
  ringCtrl->capacity = request.ringCtrlCapacity;
  ringCtrl->entrySize = sizeof(daw::EventEntry);
  ringCtrl->readIndex.store(0);
  ringCtrl->writeIndex.store(0);

  auto* ringUi = reinterpret_cast<daw::RingHeader*>(
      reinterpret_cast<uint8_t*>(state.shmBase) + header.ringUiOffset);
  ringUi->capacity = request.ringUiCapacity;
  ringUi->entrySize = sizeof(daw::EventEntry);
  ringUi->readIndex.store(0);
  ringUi->writeIndex.store(0);

  state.mailbox = reinterpret_cast<daw::BlockMailbox*>(
      reinterpret_cast<uint8_t*>(state.shmBase) + header.mailboxOffset);
  state.mailbox->completedBlockId.store(0);
  state.mailbox->completedSampleTime.store(0);
  state.mailbox->replayAckSampleTime.store(0);

  state.ringStd = daw::makeEventRing(state.shmBase, header.ringStdOffset);
  state.ringCtrl = daw::makeEventRing(state.shmBase, header.ringCtrlOffset);
  state.outputPtrs.resize(header.numChannelsOut, nullptr);
  state.inputPtrs.resize(header.numChannelsIn, nullptr);

  daw::HelloResponse response;
  response.shmSizeBytes = shmSize;
  std::snprintf(response.shmName, sizeof(response.shmName), "%s", state.shmName.c_str());

  if (!state.runtime) {
    state.runtime = daw::createJuceRuntime();
  }

  // Plugins are pre-loaded on the message thread in main() before the control loop starts.
  // This is necessary because JUCE's createPluginInstance and prepareToPlay require
  // the message thread. We do NOT re-prepare here because prepareToPlay would block
  // waiting for the message thread (which causes a deadlock from the control thread).
  // The plugins were prepared with default values (48000Hz, 512 samples) which match
  // the typical engine configuration.
  if (state.pluginPaths.empty()) {
    std::lock_guard<std::mutex> lock(state.pluginsMutex);
    state.pluginHost.reset();
    state.plugins.clear();
    state.chainBufferA.clear();
    state.chainBufferB.clear();
    state.chainPtrsA.clear();
    state.chainPtrsB.clear();
    state.pluginsReady.store(true, std::memory_order_release);
    state.pluginsLoading.store(false, std::memory_order_release);
  }
  // Note: if sample rate/block size differ from defaults, plugins may need to be
  // re-prepared. For now we trust the defaults match. A proper solution would be
  // to queue the prepare on the message thread and wait for completion.

  std::cerr << "Host: sending Hello response..." << std::endl;
  const bool sent = daw::sendMessage(state.clientFd, daw::ControlMessageType::Hello,
                                     &response, sizeof(response));
  std::cerr << "Host: Hello response sent=" << sent << std::endl;
  if (!sent) {
    return false;
  }

  return true;
}

bool handleProcessBlock(HostState& state, const daw::ProcessBlockRequest& request) {
  if (state.header == nullptr || state.mailbox == nullptr) {
    return false;
  }

  const uint64_t blockStart = request.pluginSampleStart;
  const uint64_t blockEnd = blockStart + state.header->blockSize;
  const uint32_t blockIndex = request.blockId % state.header->numBlocks;

  for (uint32_t ch = 0; ch < state.header->numChannelsOut; ++ch) {
    state.outputPtrs[ch] = daw::audioOutChannelPtr(state.shmBase,
                                                   *state.header,
                                                   blockIndex,
                                                   ch);
  }
  for (uint32_t ch = 0; ch < state.header->numChannelsIn; ++ch) {
    state.inputPtrs[ch] = daw::audioInChannelPtr(state.shmBase,
                                                 *state.header,
                                                 blockIndex,
                                                 ch);
  }

  std::unique_lock<std::mutex> pluginLock(state.pluginsMutex);
  const size_t pluginCount = state.plugins.size();
  const bool pluginsReady = state.pluginsReady.load(std::memory_order_acquire);
  const bool havePlugins = pluginCount > 0 && pluginsReady;

  if (!havePlugins) {
    const uint32_t channelsToCopy = std::min(state.header->numChannelsOut,
                                             state.header->numChannelsIn);
    if (channelsToCopy > 0) {
      for (uint32_t ch = 0; ch < channelsToCopy; ++ch) {
        std::memcpy(state.outputPtrs[ch],
                    state.inputPtrs[ch],
                    static_cast<size_t>(state.header->blockSize) * sizeof(float));
      }
    }
    for (uint32_t ch = channelsToCopy; ch < state.header->numChannelsOut; ++ch) {
      std::fill(state.outputPtrs[ch],
                state.outputPtrs[ch] + state.header->blockSize,
                0.0f);
    }
  }

  std::vector<daw::EventEntry> pending;
  pending.reserve(128);

  daw::EventEntry entry;
  auto collectEvents = [&](daw::EventRingView& ring) {
    while (daw::ringPeek(ring, entry)) {
      if (entry.sampleTime < blockStart) {
        daw::ringPop(ring, entry);
        continue;
      }
      if (entry.sampleTime >= blockEnd) {
        break;
      }
      daw::ringPop(ring, entry);
      pending.push_back(entry);
    }
  };

  collectEvents(state.ringCtrl);
  if (!state.testMode) {
    collectEvents(state.ringStd);
  }

  std::sort(pending.begin(), pending.end(),
            [](const daw::EventEntry& a, const daw::EventEntry& b) {
              if (a.sampleTime != b.sampleTime) {
                return a.sampleTime < b.sampleTime;
              }
              const int priorityA = eventPriority(a);
              const int priorityB = eventPriority(b);
              if (priorityA != priorityB) {
                return priorityA < priorityB;
              }
              return a.type < b.type;
            });

  daw::MidiEvents events;
  const bool logMidi = std::getenv("DAW_HOST_LOG_MIDI") != nullptr;
  const bool logAudio = std::getenv("DAW_HOST_LOG_AUDIO") != nullptr;
  int midiEventCount = 0;
  uint64_t replaySeenSampleTime = 0;
  for (const auto& event : pending) {
    if (event.type == static_cast<uint16_t>(daw::EventType::Transport)) {
      continue;
    }
    if (event.type == static_cast<uint16_t>(daw::EventType::ReplayComplete)) {
      replaySeenSampleTime = event.sampleTime;
      continue;
    }
    if (event.type == static_cast<uint16_t>(daw::EventType::Param) &&
        event.size >= sizeof(daw::ParamPayload)) {
      daw::ParamPayload payload{};
      std::memcpy(&payload, event.payload, sizeof(payload));
      if (havePlugins) {
        const auto uidKey = uid16Key(payload.uid16);
        if (payload.targetPluginIndex != daw::kParamTargetAll) {
          const uint32_t target = payload.targetPluginIndex;
          if (target < pluginCount) {
            auto& slot = state.plugins[target];
            const auto it = slot.paramIdByUid16.find(uidKey);
            if (it != slot.paramIdByUid16.end()) {
              slot.instance->setParameterValueNormalizedById(it->second, payload.value);
            }
          }
        } else {
          for (auto& slot : state.plugins) {
            const auto it = slot.paramIdByUid16.find(uidKey);
            if (it != slot.paramIdByUid16.end()) {
              slot.instance->setParameterValueNormalizedById(it->second, payload.value);
            }
          }
        }
      }
      continue;
    }
    if (event.type == static_cast<uint16_t>(daw::EventType::Midi) &&
        event.size >= sizeof(daw::MidiPayload)) {
      daw::MidiPayload payload{};
      std::memcpy(&payload, event.payload, sizeof(payload));
      const int offset = static_cast<int>(event.sampleTime - blockStart);
      daw::MidiEvent noteEvent;
      noteEvent.sampleOffset = offset;
      noteEvent.status = payload.status;
      noteEvent.data1 = payload.data1;
      noteEvent.data2 = payload.data2;
      noteEvent.channel = payload.channel;
      noteEvent.tuningCents = payload.tuningCents;
      noteEvent.noteId = static_cast<int32_t>(payload.noteId);
      events.push_back(noteEvent);
      ++midiEventCount;
    }
  }
  if (logMidi && midiEventCount > 0) {
    std::cerr << "Host: block " << request.blockId
              << " midiEvents=" << midiEventCount
              << " start=" << blockStart << std::endl;
  }
  if (logAudio && havePlugins && midiEventCount > 0) {
    float peak = 0.0f;
    const int channels =
        std::min(static_cast<int>(state.header->numChannelsOut),
                 static_cast<int>(state.outputPtrs.size()));
    for (int ch = 0; ch < channels; ++ch) {
      const float* data = state.outputPtrs[ch];
      if (!data) {
        continue;
      }
      for (uint32_t i = 0; i < state.header->blockSize; ++i) {
        const float v = std::abs(data[i]);
        if (v > peak) {
          peak = v;
        }
      }
    }
    std::cerr << "Host: block " << request.blockId
              << " audioPeak=" << peak << std::endl;
  }

  if (havePlugins) {
    const uint32_t totalPlugins = static_cast<uint32_t>(pluginCount);
    uint32_t segmentStart = request.segmentStart;
    uint32_t segmentLength = request.segmentLength;
    if (segmentLength == 0 || segmentStart + segmentLength > totalPlugins) {
      segmentStart = 0;
      segmentLength = totalPlugins;
    }
    if (segmentStart >= totalPlugins || segmentLength == 0) {
      segmentStart = 0;
      segmentLength = totalPlugins;
    }
    const uint32_t segmentEnd = segmentStart + segmentLength;
    const float* const* inputPtrs =
        state.header->numChannelsIn > 0 ? state.inputPtrs.data() : nullptr;
    int numInputs = static_cast<int>(state.header->numChannelsIn);
    const uint32_t channelsOut = state.header->numChannelsOut;
    for (uint32_t index = segmentStart; index < segmentEnd; ++index) {
      auto& slot = state.plugins[index];
      const bool isLast = (index + 1 == segmentEnd);
      float* const* outputPtrs = isLast ? state.outputPtrs.data()
                                        : ((index - segmentStart) % 2 == 0
                                               ? state.chainPtrsA.data()
                                               : state.chainPtrsB.data());
      const int numOutputs = static_cast<int>(channelsOut);
      const int pluginInputs = slot.instance->inputChannels();
      const float* const* pluginInputPtrs =
          pluginInputs > 0 ? inputPtrs : nullptr;
      const int pluginInputCount =
          pluginInputPtrs ? std::min(pluginInputs, numInputs) : 0;
      if (!isLast) {
        const size_t bufferSamples =
            static_cast<size_t>(channelsOut) * state.header->blockSize;
        if (((index - segmentStart) % 2) == 0) {
          std::fill(state.chainBufferA.begin(), state.chainBufferA.end(), 0.0f);
        } else {
          std::fill(state.chainBufferB.begin(), state.chainBufferB.end(), 0.0f);
        }
      }
      if (slot.bypass) {
        if (pluginInputPtrs && pluginInputCount > 0) {
          const int channelsToCopy = std::min(pluginInputCount, numOutputs);
          for (int ch = 0; ch < channelsToCopy; ++ch) {
            const float* src = pluginInputPtrs[ch];
            float* dst = outputPtrs[ch];
            std::copy(src, src + state.header->blockSize, dst);
          }
          for (int ch = channelsToCopy; ch < numOutputs; ++ch) {
            std::fill(outputPtrs[ch], outputPtrs[ch] + state.header->blockSize, 0.0f);
          }
        } else {
          for (int ch = 0; ch < numOutputs; ++ch) {
            std::fill(outputPtrs[ch], outputPtrs[ch] + state.header->blockSize, 0.0f);
          }
        }
      } else {
        slot.instance->process(pluginInputPtrs,
                               pluginInputCount,
                               outputPtrs,
                               numOutputs,
                               static_cast<int>(state.header->blockSize),
                               events,
                               static_cast<int64_t>(blockStart));
      }
      inputPtrs = outputPtrs;
      numInputs = numOutputs;
    }
  }

  if (state.mailbox) {
    bool isLastSegment = true;
    if (havePlugins) {
      const uint32_t totalPlugins = static_cast<uint32_t>(pluginCount);
      uint32_t segmentStart = request.segmentStart;
      uint32_t segmentLength = request.segmentLength;
      if (segmentLength == 0 || segmentStart + segmentLength > totalPlugins) {
        segmentStart = 0;
        segmentLength = totalPlugins;
      }
      if (segmentStart >= totalPlugins || segmentLength == 0) {
        segmentStart = 0;
        segmentLength = totalPlugins;
      }
      isLastSegment = (segmentStart + segmentLength >= totalPlugins);
    }
    if (isLastSegment) {
      state.mailbox->completedSampleTime.store(request.engineSampleStart,
                                               std::memory_order_release);
      state.mailbox->completedBlockId.store(request.blockId,
                                            std::memory_order_release);
    }
    if (replaySeenSampleTime > 0) {
      const uint64_t current =
          state.mailbox->replayAckSampleTime.load(std::memory_order_relaxed);
      const uint64_t next = replaySeenSampleTime > current ? replaySeenSampleTime : current;
      state.mailbox->replayAckSampleTime.store(next, std::memory_order_release);
    }
  }
  return true;
}

bool handleOpenEditor(HostState& state, const daw::OpenEditorRequest& request) {
  std::lock_guard<std::mutex> lock(state.pluginsMutex);
  if (request.pluginIndex >= state.plugins.size()) {
    std::cerr << "Host: invalid plugin index " << request.pluginIndex << std::endl;
    return true;
  }
  auto& slot = state.plugins[request.pluginIndex];
  if (!slot.instance) {
    return true;
  }
  if (!slot.instance->openEditor()) {
    std::cerr << "Host: failed to open editor for plugin "
              << request.pluginIndex << std::endl;
  }
  return true;
}

bool handleSetBypass(HostState& state, const daw::SetBypassRequest& request) {
  std::lock_guard<std::mutex> lock(state.pluginsMutex);
  if (request.pluginIndex >= state.plugins.size()) {
    return true;
  }
  state.plugins[request.pluginIndex].bypass = request.bypass != 0;
  return true;
}

void cleanup(HostState& state, const std::string& socketPath) {
  if (state.shmBase && state.shmBase != MAP_FAILED) {
    ::munmap(state.shmBase, state.shmSize);
    state.shmBase = nullptr;
  }
  if (!state.shmName.empty()) {
    ::shm_unlink(state.shmName.c_str());
  }
  closeFd(state.shmFd);
  closeFd(state.clientFd);
  closeFd(state.serverFd);
  ::unlink(socketPath.c_str());
}

void requestStopDispatchLoop() {
  auto* manager = juce::MessageManager::getInstanceWithoutCreating();
  if (!manager) {
    return;
  }
  juce::MessageManager::callAsync([]() {
    if (auto* inner = juce::MessageManager::getInstanceWithoutCreating()) {
      inner->stopDispatchLoop();
    }
  });
}

void runControlLoop(HostState& state) {
  while (true) {
    daw::ControlHeader header;
    if (!daw::recvHeader(state.clientFd, header)) {
      if (!state.testMode) {
        std::cerr << "Failed to receive control header." << std::endl;
      }
      break;
    }

    if (header.size > 0) {
      std::vector<uint8_t> payload(header.size);
      if (!daw::readAll(state.clientFd, payload.data(), payload.size())) {
        break;
      }
      const auto type = static_cast<daw::ControlMessageType>(header.type);
      if (type == daw::ControlMessageType::Hello) {
        if (payload.size() == sizeof(daw::HelloRequest)) {
          const auto* request =
              reinterpret_cast<const daw::HelloRequest*>(payload.data());
          if (!handleHello(state, *request)) {
            break;
          }
        }
      } else if (type == daw::ControlMessageType::ProcessBlock) {
        if (payload.size() == sizeof(daw::ProcessBlockRequest)) {
          const auto* request =
              reinterpret_cast<const daw::ProcessBlockRequest*>(payload.data());
          if (!handleProcessBlock(state, *request)) {
            break;
          }
        }
      } else if (type == daw::ControlMessageType::OpenEditor) {
        if (payload.size() == sizeof(daw::OpenEditorRequest)) {
          const auto* request =
              reinterpret_cast<const daw::OpenEditorRequest*>(payload.data());
          if (!handleOpenEditor(state, *request)) {
            break;
          }
        }
      } else if (type == daw::ControlMessageType::SetBypass) {
        if (payload.size() == sizeof(daw::SetBypassRequest)) {
          const auto* request =
              reinterpret_cast<const daw::SetBypassRequest*>(payload.data());
          if (!handleSetBypass(state, *request)) {
            break;
          }
        }
      } else if (type == daw::ControlMessageType::Shutdown) {
        break;
      }
    }
  }
  requestStopDispatchLoop();
}

}  // namespace

int main(int argc, char** argv) {
  std::string socketPath = "/tmp/daw_host.sock";
  HostState state;
  if (const char* env = std::getenv("DAW_ENGINE_TEST_MODE")) {
    state.testMode = std::string(env) == "1";
  }
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--socket") {
      socketPath = argv[i + 1];
      ++i;
    } else if (std::string(argv[i]) == "--plugin") {
      state.pluginPaths.push_back(argv[i + 1]);
      ++i;
    }
  }
  std::cerr << "Host: plugin paths=" << state.pluginPaths.size() << std::endl;
  state.serverFd = createServerSocket(socketPath);
  if (state.serverFd < 0) {
    std::cerr << "Failed to create server socket: " << socketPath << std::endl;
    return 1;
  }

  state.clientFd = ::accept(state.serverFd, nullptr, nullptr);
  if (state.clientFd < 0) {
    std::cerr << "Failed to accept client connection." << std::endl;
    cleanup(state, socketPath);
    return 1;
  }

  state.runtime = daw::createJuceRuntime();
  auto* manager = juce::MessageManager::getInstance();
  if (!manager) {
    runControlLoop(state);
    cleanup(state, socketPath);
    return 0;
  }
  manager->setCurrentThreadAsMessageThread();

  // Pre-load plugins on the message thread BEFORE starting the control loop.
  // JUCE's createPluginInstance requires the message thread, so we can't load
  // from the control thread without blocking. Load now while we're on main thread.
  if (!state.pluginPaths.empty()) {
    // Default audio params - will be updated when Hello arrives
    const double defaultSampleRate = 48000.0;
    const uint32_t defaultBlockSize = 512;
    const uint32_t defaultChannels = 2;
    loadPlugins(state, state.pluginPaths, defaultSampleRate, defaultBlockSize, defaultChannels);
  }

  std::thread controlThread([&state]() { runControlLoop(state); });
  manager->runDispatchLoop();
  if (controlThread.joinable()) {
    controlThread.join();
  }

  cleanup(state, socketPath);
  return 0;
}
