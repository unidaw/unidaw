#include <cassert>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/device_chain.h"
#include "apps/plugin_cache.h"
#include "apps/shared_memory.h"
#include "apps/audio_shm.h"

namespace {

std::string makeName(const char* prefix, pid_t pid) {
  return std::string(prefix) + "_" + std::to_string(pid);
}

bool waitForShm(const std::string& name, int& fd, size_t& size, void*& base) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd >= 0) {
      struct stat st{};
      if (::fstat(fd, &st) == 0 && st.st_size > 0) {
        size = static_cast<size_t>(st.st_size);
        base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base != MAP_FAILED) {
          return true;
        }
      }
      ::close(fd);
      fd = -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool waitForNoteDiff(daw::EventRingView& ringOut,
                     uint32_t expectedPitch,
                     std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  daw::EventEntry entry{};
  while (std::chrono::steady_clock::now() - start < timeout) {
    while (daw::ringPeek(ringOut, entry)) {
      daw::ringPop(ringOut, entry);
      if (entry.type != static_cast<uint16_t>(daw::EventType::UiDiff)) {
        continue;
      }
      if (entry.size != sizeof(daw::UiDiffPayload)) {
        continue;
      }
      daw::UiDiffPayload diff{};
      std::memcpy(&diff, entry.payload, sizeof(diff));
      if (diff.diffType == static_cast<uint16_t>(daw::UiDiffType::AddNote) &&
          diff.notePitch == expectedPitch) {
        return true;
      }
      if (diff.diffType == static_cast<uint16_t>(daw::UiDiffType::ResyncNeeded)) {
        return false;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool waitForAudioNonZero(const daw::ShmHeader* header,
                         void* base,
                         const daw::EventRingView& ringStd,
                         std::chrono::milliseconds timeout) {
  if (!header || !base) {
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto* mailbox = reinterpret_cast<const daw::BlockMailbox*>(
      reinterpret_cast<const uint8_t*>(base) + header->mailboxOffset);
  uint32_t lastBlock = mailbox ? mailbox->completedBlockId.load() : 0;
  uint32_t lastWrite = ringStd.header ? ringStd.header->writeIndex.load() : 0;
  bool ringStdAdvanced = false;
  bool midiSeen = false;
  bool mailboxAdvanced = false;
  const float epsilon = 1e-6f;
  while (std::chrono::steady_clock::now() - start < timeout) {
    if (!mailbox) {
      return false;
    }
    if (ringStd.header && ringStd.mask != 0) {
      const uint32_t write = ringStd.header->writeIndex.load();
      const uint32_t read = ringStd.header->readIndex.load();
      if (write != lastWrite) {
        ringStdAdvanced = true;
        lastWrite = write;
      }
      uint32_t pending = write - read;
      if (pending > ringStd.header->capacity) {
        pending = ringStd.header->capacity;
      }
      for (uint32_t i = 0; i < pending; ++i) {
        const uint32_t index = (read + i) & ringStd.mask;
        const daw::EventEntry& entry = ringStd.entries[index];
        if (entry.type == static_cast<uint16_t>(daw::EventType::Midi)) {
          midiSeen = true;
          break;
        }
      }
    }
    const uint32_t blockId = mailbox->completedBlockId.load();
    if (blockId != lastBlock) {
      mailboxAdvanced = true;
      lastBlock = blockId;
    }
    for (uint32_t blockIndex = 0; blockIndex < header->numBlocks; ++blockIndex) {
      bool anyNonZero = false;
      for (uint32_t ch = 0; ch < header->numChannelsOut; ++ch) {
        float* channel = daw::audioOutChannelPtr(
            base, *header, blockIndex, ch);
        if (!channel) {
          continue;
        }
        for (uint32_t i = 0; i < header->blockSize; ++i) {
          if (std::abs(channel[i]) > epsilon) {
            anyNonZero = true;
            break;
          }
        }
        if (anyNonZero) {
          break;
        }
      }
      if (anyNonZero) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (ringStd.header && !ringStdAdvanced) {
    std::cerr << "device_chain_ui_live_tests_main: ringStd never advanced"
              << std::endl;
  }
  if (ringStd.header && !midiSeen) {
    std::cerr << "device_chain_ui_live_tests_main: no MIDI events observed"
              << std::endl;
  }
  if (!mailboxAdvanced) {
    std::cerr << "device_chain_ui_live_tests_main: host mailbox never advanced"
              << std::endl;
  }
  return false;
}

bool injectTestMidi(daw::EventRingView& ringStd,
                    const daw::ShmHeader* header,
                    const daw::BlockMailbox* mailbox) {
  if (!header || !mailbox) {
    return false;
  }
  const uint64_t engineSampleStart =
      mailbox->completedSampleTime.load(std::memory_order_acquire);
  const uint64_t latencySamples =
      static_cast<uint64_t>(
          header->numBlocks > 0 ? (header->numBlocks - 1) * header->blockSize : 0);
  const uint64_t nextEngineStart = engineSampleStart + header->blockSize;
  const uint64_t pluginStart =
      nextEngineStart > latencySamples ? nextEngineStart - latencySamples : 0;

  daw::EventEntry entry{};
  entry.sampleTime = pluginStart + 1;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::Midi);
  entry.size = sizeof(daw::MidiPayload);
  daw::MidiPayload payload{};
  payload.status = 0x90;
  payload.data1 = 60;
  payload.data2 = 100;
  payload.channel = 0;
  payload.tuningCents = 0.0f;
  payload.noteId = 1;
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ringStd, entry);
}

bool sendUiCommand(daw::EventRingView& ring, const daw::UiCommandPayload& payload) {
  daw::EventEntry entry{};
  entry.sampleTime = 0;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  entry.size = sizeof(daw::UiCommandPayload);
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
}

bool sendChainCommand(daw::EventRingView& ring, const daw::UiChainCommandPayload& payload) {
  daw::EventEntry entry{};
  entry.sampleTime = 0;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  entry.size = sizeof(daw::UiChainCommandPayload);
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
}

bool waitForPlayheadAdvance(daw::ShmHeader* header,
                            std::chrono::milliseconds timeout) {
  if (!header) {
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  const uint64_t initial = header->uiGlobalNanotickPlayhead;
  const bool debug = std::getenv("DAW_TEST_DEBUG") != nullptr;
  while (std::chrono::steady_clock::now() - start < timeout) {
    const uint64_t current = header->uiGlobalNanotickPlayhead;
    if (current != initial) {
      if (debug) {
        std::cerr << "device_chain_ui_live_tests_main: playhead advanced "
                  << initial << " -> " << current << std::endl;
      }
      return true;
    }
    if (debug) {
      const uint64_t version = header->uiVersion.load();
      std::cerr << "device_chain_ui_live_tests_main: playhead="
                << current << " uiVersion=" << version << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

int main() {
  const pid_t pid = ::getpid();
  const std::string uiShmName = makeName("/daw_engine_ui_live", pid);
  const std::string socketPrefix = makeName("/tmp/daw_host_live", pid);

  pid_t child = ::fork();
  if (child == 0) {
    ::setenv("DAW_UI_SHM_NAME", uiShmName.c_str(), 1);
    ::setenv("DAW_HOST_SOCKET_PREFIX", socketPrefix.c_str(), 1);
    ::unsetenv("DAW_ENGINE_TEST_MODE");
    ::unsetenv("DAW_ENGINE_TEST_THROTTLE_MS");
    const char* exe = "./daw_engine";
    const char* arg0 = "daw_engine";
    const char* arg1 = "--run-seconds";
    const char* arg2 = "6";
    ::execl(exe, arg0, arg1, arg2, nullptr);
    _exit(1);
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));

  int fd = -1;
  size_t size = 0;
  void* base = nullptr;
  bool mapped = waitForShm(uiShmName, fd, size, base);
  if (!mapped) {
    std::cerr << "device_chain_ui_live_tests_main: failed to map UI SHM"
              << std::endl;
    return 1;
  }

  auto* header = reinterpret_cast<daw::ShmHeader*>(base);
  daw::EventRingView ringUi = daw::makeEventRing(base, header->ringUiOffset);
  if (ringUi.mask == 0) {
    std::cerr << "device_chain_ui_live_tests_main: invalid UI ring mask"
              << std::endl;
    return 1;
  }
  if (const char* env = std::getenv("DAW_TEST_DEBUG")) {
    std::cerr << "device_chain_ui_live_tests_main: ringUiOffset="
              << header->ringUiOffset
              << " capacity=" << ringUi.header->capacity
              << " entrySize=" << ringUi.header->entrySize
              << " mask=" << ringUi.mask
              << " read=" << ringUi.header->readIndex.load()
              << " write=" << ringUi.header->writeIndex.load()
              << std::endl;
  }

  daw::EventRingView ringUiOut = daw::makeEventRing(base, header->ringUiOutOffset);
  if (ringUiOut.mask == 0) {
    std::cerr << "device_chain_ui_live_tests_main: invalid UI out ring mask"
              << std::endl;
    return 1;
  }

  int hostFd = -1;
  size_t hostSize = 0;
  void* hostBase = nullptr;
  if (!waitForShm("/daw_engine_shared", hostFd, hostSize, hostBase)) {
    std::cerr << "device_chain_ui_live_tests_main: failed to map host SHM"
              << std::endl;
    return 1;
  }
  auto* hostHeader = reinterpret_cast<daw::ShmHeader*>(hostBase);
  daw::EventRingView ringStd =
      daw::makeEventRing(hostBase, hostHeader->ringStdOffset);
  if (ringStd.mask == 0) {
    std::cerr << "device_chain_ui_live_tests_main: invalid ringStd mask"
              << std::endl;
    return 1;
  }
  const auto* mailbox = reinterpret_cast<const daw::BlockMailbox*>(
      reinterpret_cast<const uint8_t*>(hostBase) + hostHeader->mailboxOffset);

  daw::UiCommandPayload playPayload{};
  playPayload.commandType =
      static_cast<uint16_t>(daw::UiCommandType::TogglePlay);
  playPayload.trackId = 0;

  daw::PluginCache pluginCache = daw::readPluginCache("plugin_cache.json");
  if (pluginCache.entries.empty()) {
    std::cerr << "device_chain_ui_live_tests_main: plugin cache empty"
              << std::endl;
    return 1;
  }
  std::optional<uint32_t> identityIndex;
  for (size_t i = 0; i < pluginCache.entries.size(); ++i) {
    const auto& entry = pluginCache.entries[i];
    if (!entry.isInstrument) {
      continue;
    }
    if (entry.path.find("Identity.vst3") != std::string::npos) {
      identityIndex = static_cast<uint32_t>(i);
      break;
    }
  }

  if (identityIndex) {
    daw::UiCommandPayload loadPayload{};
    loadPayload.commandType =
        static_cast<uint16_t>(daw::UiCommandType::LoadPluginOnTrack);
    loadPayload.trackId = 0;
    loadPayload.pluginIndex = *identityIndex;
    if (!sendUiCommand(ringUi, loadPayload)) {
      std::cerr << "device_chain_ui_live_tests_main: failed to send load"
                << std::endl;
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  daw::UiCommandPayload notePayload{};
  notePayload.commandType =
      static_cast<uint16_t>(daw::UiCommandType::WriteNote);
  notePayload.trackId = 0;
  notePayload.notePitch = 60;
  notePayload.value0 = 100;
  const uint64_t rowNanoticks = 240000;
  const uint64_t patternTicks = rowNanoticks * 16;
  const uint64_t noteNanotick =
      (header->uiGlobalNanotickPlayhead + rowNanoticks) % patternTicks;
  notePayload.noteNanotickLo =
      static_cast<uint32_t>(noteNanotick & 0xffffffffu);
  notePayload.noteNanotickHi =
      static_cast<uint32_t>((noteNanotick >> 32) & 0xffffffffu);
  notePayload.noteDurationLo = 0;
  notePayload.noteDurationHi = 0;
  notePayload.baseVersion = header->uiClipVersion;
  if (!sendUiCommand(ringUi, notePayload)) {
    std::cerr << "device_chain_ui_live_tests_main: failed to send note"
              << std::endl;
    return 1;
  }
  if (!waitForNoteDiff(ringUiOut, notePayload.notePitch,
                       std::chrono::milliseconds(500))) {
    std::cerr << "device_chain_ui_live_tests_main: note diff missing"
              << std::endl;
    return 1;
  }

  const bool playAfterSent = sendUiCommand(ringUi, playPayload);
  if (!playAfterSent) {
    std::cerr << "device_chain_ui_live_tests_main: failed to send play after load" << std::endl;
    return 1;
  }
  if (const char* env = std::getenv("DAW_TEST_DEBUG")) {
    const uint32_t read = ringUi.header->readIndex.load();
    const uint32_t write = ringUi.header->writeIndex.load();
    std::cerr << "device_chain_ui_live_tests_main: sent play after load (read="
              << read << ", write=" << write << ")" << std::endl;
  }

  if (!waitForPlayheadAdvance(header, std::chrono::milliseconds(800))) {
    std::cerr << "device_chain_ui_live_tests_main: playhead did not advance"
              << std::endl;
    return 1;
  }

  bool injected = false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (mailbox && mailbox->completedBlockId.load() > 0) {
      injected = injectTestMidi(ringStd, hostHeader, mailbox);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!injected) {
    std::cerr << "device_chain_ui_live_tests_main: failed to inject MIDI"
              << std::endl;
    return 1;
  }

  if (!waitForAudioNonZero(hostHeader, hostBase, ringStd,
                           std::chrono::milliseconds(1500))) {
    std::cerr << "device_chain_ui_live_tests_main: no audio after note"
              << std::endl;
    return 1;
  }

  if (identityIndex) {
    daw::UiChainCommandPayload addPayload{};
    addPayload.commandType = static_cast<uint16_t>(daw::UiCommandType::AddDevice);
    addPayload.trackId = 0;
    addPayload.deviceKind = static_cast<uint32_t>(daw::DeviceKind::VstEffect);
    addPayload.hostSlotIndex = *identityIndex;
    addPayload.insertIndex = daw::kChainDeviceIdAuto;
    if (!sendChainCommand(ringUi, addPayload)) {
      std::cerr << "device_chain_ui_live_tests_main: failed to send add device"
                << std::endl;
      return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const uint64_t tick3 = header->uiGlobalNanotickPlayhead;
    if (!waitForPlayheadAdvance(header, std::chrono::milliseconds(800))) {
      std::cerr << "device_chain_ui_live_tests_main: playhead stalled after load"
                << std::endl;
      return 1;
    }
    const uint64_t tick4 = header->uiGlobalNanotickPlayhead;
    if (tick4 <= tick3) {
      std::cerr << "device_chain_ui_live_tests_main: playhead not advancing"
                << std::endl;
      return 1;
    }
  }

  int status = 0;
  ::waitpid(child, &status, 0);
  if (!WIFEXITED(status)) {
    std::cerr << "device_chain_ui_live_tests_main: engine did not exit cleanly"
              << std::endl;
    return 1;
  }

  if (base && base != MAP_FAILED) {
    ::munmap(base, size);
  }
  if (hostBase && hostBase != MAP_FAILED) {
    ::munmap(hostBase, hostSize);
  }
  if (fd >= 0) {
    ::close(fd);
  }
  if (hostFd >= 0) {
    ::close(hostFd);
  }
  ::shm_unlink(uiShmName.c_str());

  std::cout << "device_chain_ui_live_tests_main: ok" << std::endl;
  return 0;
}
