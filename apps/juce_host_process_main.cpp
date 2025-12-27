#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>
#include <array>
#include <unordered_map>
#include <algorithm>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "apps/audio_shm.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/ipc_protocol.h"
#include "apps/ipc_io.h"
#include "apps/shared_memory.h"
#include "platform_juce/juce_wrapper.h"
#include "platform_juce/uid_utils.h"

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
  std::unique_ptr<daw::IPluginInstance> plugin;
  std::vector<float*> inputPtrs;
  std::vector<float*> outputPtrs;
  std::string pluginPath;
  std::unordered_map<std::string, std::string> paramIdByUid16;
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
  const int pid = static_cast<int>(::getpid());
  return "/daw_host_" + std::to_string(pid);
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

bool handleHello(HostState& state, const daw::HelloRequest& request) {
  state.shmName = makeShmName();
  state.shmFd = ::shm_open(state.shmName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (state.shmFd < 0) {
    return false;
  }

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
                                               request.ringCtrlCapacity);
  if (::ftruncate(state.shmFd, static_cast<off_t>(shmSize)) != 0) {
    return false;
  }

  state.shmBase = ::mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                         state.shmFd, 0);
  if (state.shmBase == MAP_FAILED) {
    state.shmBase = nullptr;
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
  header.mailboxOffset = offset;

  std::memcpy(state.header, &header, sizeof(header));
  state.header->uiVisualSampleCount = 0;

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

  state.mailbox = reinterpret_cast<daw::BlockMailbox*>(
      reinterpret_cast<uint8_t*>(state.shmBase) + header.mailboxOffset);
  state.mailbox->completedBlockId.store(0);
  state.mailbox->completedSampleTime.store(0);

  state.ringStd = daw::makeEventRing(state.shmBase, header.ringStdOffset);
  state.ringCtrl = daw::makeEventRing(state.shmBase, header.ringCtrlOffset);
  state.outputPtrs.resize(header.numChannelsOut, nullptr);
  state.inputPtrs.resize(header.numChannelsIn, nullptr);

  if (!state.pluginPath.empty()) {
    if (!std::filesystem::exists(state.pluginPath)) {
      std::cerr << "Plugin path not found: " << state.pluginPath << std::endl;
    }
    state.runtime = daw::createJuceRuntime();
    state.pluginHost = daw::createPluginHost();
    state.plugin = state.pluginHost->loadVst3FromPath(
        state.pluginPath, request.sampleRate, request.blockSize);
    if (state.plugin) {
      state.plugin->prepare(request.sampleRate, request.blockSize,
                            static_cast<int>(request.numChannelsOut));
      state.paramIdByUid16.clear();
      for (const auto& param : state.plugin->parameters()) {
        const auto uid16 = daw::md5Uid16FromIdentifier(param.stableId);
        state.paramIdByUid16.emplace(
            uid16Key(uid16.data()), param.stableId);
      }
    } else {
      std::cerr << "Failed to load plugin in host process: " << state.pluginPath
                << std::endl;
    }
  }

  daw::HelloResponse response;
  response.shmSizeBytes = shmSize;
  std::snprintf(response.shmName, sizeof(response.shmName), "%s", state.shmName.c_str());

  return daw::sendMessage(state.clientFd, daw::ControlMessageType::Hello,
                          &response, sizeof(response));
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

        if (!state.plugin) {

          const uint32_t channelsToCopy = std::min(state.header->numChannelsOut,

                                                   state.header->numChannelsIn);

          if (channelsToCopy > 0) {

            for (uint32_t ch = 0; ch < channelsToCopy; ++ch) {

              std::memcpy(state.outputPtrs[ch],

                          state.inputPtrs[ch],

                          static_cast<size_t>(state.header->blockSize) * sizeof(float));

            }      for (uint32_t ch = channelsToCopy; ch < state.header->numChannelsOut; ++ch) {
        std::fill(state.outputPtrs[ch],
                  state.outputPtrs[ch] + state.header->blockSize,
                  0.0f);
      }
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
  collectEvents(state.ringStd);

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
  for (const auto& event : pending) {
    if (event.type == static_cast<uint16_t>(daw::EventType::Transport)) {
      continue;
    }
    if (event.type == static_cast<uint16_t>(daw::EventType::Param) &&
        event.size >= sizeof(daw::ParamPayload)) {
      daw::ParamPayload payload{};
      std::memcpy(&payload, event.payload, sizeof(payload));
      const auto it = state.paramIdByUid16.find(uid16Key(payload.uid16));
      if (it != state.paramIdByUid16.end() && state.plugin) {
        state.plugin->setParameterValueNormalizedById(it->second, payload.value);
      }
      continue;
    }
    if (event.type == static_cast<uint16_t>(daw::EventType::Midi) &&
        event.size >= sizeof(daw::MidiPayload)) {
      daw::MidiPayload payload{};
      std::memcpy(&payload, event.payload, sizeof(payload));
      const int offset = static_cast<int>(event.sampleTime - blockStart);
      events.push_back({offset, payload.status, payload.data1, payload.data2});
    }
  }

  if (state.plugin) {
    const float* const* inputPtrs =
        state.header->numChannelsIn > 0 ? state.inputPtrs.data() : nullptr;
    state.plugin->process(inputPtrs,
                          static_cast<int>(state.header->numChannelsIn),
                          state.outputPtrs.data(),
                          static_cast<int>(state.header->numChannelsOut),
                          static_cast<int>(state.header->blockSize),
                          events,
                          static_cast<int64_t>(blockStart));
  }

  if (state.mailbox) {
    state.mailbox->completedSampleTime.store(request.engineSampleStart,
                                             std::memory_order_release);
    state.mailbox->completedBlockId.store(request.blockId,
                                          std::memory_order_release);
  }
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

}  // namespace

int main(int argc, char** argv) {
  std::string socketPath = "/tmp/daw_host.sock";
  HostState state;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--socket") {
      socketPath = argv[i + 1];
      ++i;
    } else if (std::string(argv[i]) == "--plugin") {
      state.pluginPath = argv[i + 1];
      ++i;
    }
  }
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

  while (true) {
    daw::ControlHeader header;
    if (!daw::recvHeader(state.clientFd, header)) {
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
      } else if (type == daw::ControlMessageType::Shutdown) {
        break;
      }
    }
  }

  cleanup(state, socketPath);
  return 0;
}
