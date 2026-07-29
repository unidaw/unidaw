#include <cassert>
#include <chrono>
#include <cstring>
#include <set>
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
#include "apps/modulation.h"
#include "apps/patcher_graph.h"
#include "apps/plugin_cache.h"
#include "apps/track_routing.h"
#include "apps/shared_memory.h"

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

bool sendChainCommand(daw::EventRingView& ring, const daw::UiChainCommandPayload& payload) {
  daw::EventEntry entry{};
  entry.sampleTime = 0;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  entry.size = sizeof(daw::UiChainCommandPayload);
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
}

bool sendRoutingCommand(daw::EventRingView& ring,
                        const daw::UiTrackRoutingPayload& payload) {
  daw::EventEntry entry{};
  entry.sampleTime = 0;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  entry.size = sizeof(daw::UiTrackRoutingPayload);
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
}

bool sendModLinkCommand(daw::EventRingView& ring,
                        const daw::UiModLinkCommandPayload& payload) {
  daw::EventEntry entry{};
  entry.sampleTime = 0;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  entry.size = sizeof(daw::UiModLinkCommandPayload);
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
}

bool sendModLinkUid16Command(daw::EventRingView& ring,
                             const daw::UiModLinkUid16Payload& payload) {
  daw::EventEntry entry{};
  entry.sampleTime = 0;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  entry.size = sizeof(daw::UiModLinkUid16Payload);
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
}

bool sendPatcherGraphCommand(daw::EventRingView& ring,
                             const daw::UiPatcherGraphCommandPayload& payload) {
  daw::EventEntry entry{};
  entry.sampleTime = 0;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  entry.size = sizeof(daw::UiPatcherGraphCommandPayload);
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
}

bool sendPatcherConfigCommand(daw::EventRingView& ring,
                              const daw::UiPatcherNodeConfigPayload& payload) {
  daw::EventEntry entry{};
  entry.sampleTime = 0;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::UiCommand);
  entry.size = sizeof(daw::UiPatcherNodeConfigPayload);
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
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

bool waitForPlayheadAdvance(daw::ShmHeader* header,
                            std::chrono::milliseconds timeout) {
  if (!header) {
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  const uint64_t initial =
      header->uiGlobalNanotickPlayhead;
  while (std::chrono::steady_clock::now() - start < timeout) {
    const uint64_t current =
        header->uiGlobalNanotickPlayhead;
    if (current != initial) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

int main() {
  const pid_t pid = ::getpid();
  const std::string uiShmName = makeName("/daw_engine_ui_test", pid);
  const std::string socketPrefix = makeName("/tmp/daw_host_test", pid);

  pid_t child = ::fork();
  if (child == 0) {
    ::setenv("DAW_ENGINE_TEST_MODE", "1", 1);
    ::setenv("DAW_ENGINE_TEST_THROTTLE_MS", "5", 1);
    ::setenv("DAW_UI_SHM_NAME", uiShmName.c_str(), 1);
    ::setenv("DAW_HOST_SOCKET_PREFIX", socketPrefix.c_str(), 1);
    const char* exe = "./daw_engine";
    const char* arg0 = "daw_engine";
    const char* arg1 = "--run-seconds";
    const char* arg2 = "2";
    ::execl(exe, arg0, arg1, arg2, nullptr);
    _exit(1);
  }

  int fd = -1;
  size_t size = 0;
  void* base = nullptr;
  bool mapped = waitForShm(uiShmName, fd, size, base);
  assert(mapped);

  auto* header = reinterpret_cast<daw::ShmHeader*>(base);
  daw::EventRingView ringUi = daw::makeEventRing(base, header->ringUiOffset);
  assert(ringUi.mask != 0);

  daw::UiChainCommandPayload addPayload{};
  addPayload.commandType = static_cast<uint16_t>(daw::UiCommandType::AddDevice);
  addPayload.trackId = 0;
  addPayload.deviceId = 42;
  addPayload.deviceKind = static_cast<uint32_t>(daw::DeviceKind::PatcherAudio);
  addPayload.insertIndex = 1;
  addPayload.patcherNodeId = 0;
  addPayload.hostSlotIndex = 0;
  addPayload.bypass = 0;
  assert(sendChainCommand(ringUi, addPayload));

  daw::UiChainCommandPayload movePayload = addPayload;
  movePayload.commandType = static_cast<uint16_t>(daw::UiCommandType::MoveDevice);
  movePayload.insertIndex = 3;
  assert(sendChainCommand(ringUi, movePayload));

  daw::UiChainCommandPayload updatePayload = addPayload;
  updatePayload.commandType = static_cast<uint16_t>(daw::UiCommandType::UpdateDevice);
  updatePayload.flags = 0x1u;
  updatePayload.bypass = 1;
  assert(sendChainCommand(ringUi, updatePayload));

  daw::UiChainCommandPayload removePayload = addPayload;
  removePayload.commandType = static_cast<uint16_t>(daw::UiCommandType::RemoveDevice);
  assert(sendChainCommand(ringUi, removePayload));

  daw::UiChainCommandPayload badRemove = addPayload;
  badRemove.commandType = static_cast<uint16_t>(daw::UiCommandType::RemoveDevice);
  badRemove.deviceId = 9999;
  assert(sendChainCommand(ringUi, badRemove));

  // A mod link needs two devices that EXIST, with the source ahead of the target in
  // the chain — the engine validates both and answers ModError otherwise. The link
  // below used to name devices 0 and 1, which nothing had created (device 42 was added
  // and then removed above), so the engine rejected it and no ModSnapshot was ever
  // emitted. The test could not tell, because it dispatched diffs by size and its
  // asserts were compiled out. Create the two devices the link refers to.
  daw::UiChainCommandPayload modSourceDevice = addPayload;
  modSourceDevice.deviceId = 10;
  modSourceDevice.insertIndex = 0;
  assert(sendChainCommand(ringUi, modSourceDevice));
  daw::UiChainCommandPayload modTargetDevice = addPayload;
  modTargetDevice.deviceId = 11;
  modTargetDevice.insertIndex = 1;
  assert(sendChainCommand(ringUi, modTargetDevice));

  daw::UiTrackRoutingPayload routingPayload{};
  routingPayload.commandType =
      static_cast<uint16_t>(daw::UiCommandType::SetTrackRouting);
  routingPayload.trackId = 0;
  routingPayload.flags = 0x1u;
  routingPayload.midiOutKind = static_cast<uint8_t>(daw::TrackRouteKind::Track);
  routingPayload.audioOutKind = static_cast<uint8_t>(daw::TrackRouteKind::Track);
  routingPayload.midiOutTrackId = 1;
  routingPayload.audioOutTrackId = 1;
  assert(sendRoutingCommand(ringUi, routingPayload));

  daw::UiModLinkCommandPayload modPayload{};
  modPayload.commandType = static_cast<uint16_t>(daw::UiCommandType::AddModLink);
  modPayload.trackId = 0;
  modPayload.linkId = 7;
  modPayload.sourceDeviceId = 10;
  modPayload.sourceId = 0;
  modPayload.targetDeviceId = 11;
  modPayload.targetId = 0;
  modPayload.depth = 0.5f;
  modPayload.bias = 0.1f;
  const uint16_t modFlags =
      static_cast<uint16_t>(daw::ModSourceKind::Macro) |
      (static_cast<uint16_t>(daw::ModTargetKind::VstParam) << 4) |
      (static_cast<uint16_t>(daw::ModRate::BlockRate) << 8) |
      (1u << 10);
  modPayload.flags = modFlags;
  assert(sendModLinkCommand(ringUi, modPayload));

  daw::UiModLinkUid16Payload modUidPayload{};
  modUidPayload.commandType =
      static_cast<uint16_t>(daw::UiCommandType::SetModLinkUid16);
  modUidPayload.trackId = 0;
  modUidPayload.linkId = 7;
  for (size_t i = 0; i < sizeof(modUidPayload.uid16); ++i) {
    modUidPayload.uid16[i] = static_cast<uint8_t>(i);
  }
  assert(sendModLinkUid16Command(ringUi, modUidPayload));

  daw::UiPatcherGraphCommandPayload addNode{};
  addNode.commandType = static_cast<uint16_t>(daw::UiCommandType::AddPatcherNode);
  addNode.trackId = 0;
  addNode.nodeType = static_cast<uint32_t>(daw::PatcherNodeType::Lfo);
  assert(sendPatcherGraphCommand(ringUi, addNode));

  daw::UiPatcherGraphCommandPayload connectNode{};
  connectNode.commandType =
      static_cast<uint16_t>(daw::UiCommandType::ConnectPatcherNodes);
  connectNode.trackId = 0;
  connectNode.srcNodeId = 0;
  connectNode.dstNodeId = 1;
  connectNode.srcPortId = daw::kPatcherEventOutputPort;
  connectNode.dstPortId = daw::kPatcherEventInputPort;
  connectNode.edgeKind = static_cast<uint32_t>(daw::PatcherPortKind::Event);
  assert(sendPatcherGraphCommand(ringUi, connectNode));

  daw::PatcherLfoConfig lfoConfig{};
  lfoConfig.frequency_hz = 2.0f;
  lfoConfig.depth = 0.5f;
  lfoConfig.bias = 0.1f;
  lfoConfig.phase_offset = 0.0f;
  daw::UiPatcherNodeConfigPayload configPayload{};
  configPayload.commandType =
      static_cast<uint16_t>(daw::UiCommandType::SetPatcherNodeConfig);
  configPayload.trackId = 0;
  configPayload.nodeId = 1;
  configPayload.configType = static_cast<uint32_t>(daw::PatcherNodeType::Lfo);
  std::memcpy(configPayload.config, &lfoConfig, sizeof(lfoConfig));
  assert(sendPatcherConfigCommand(ringUi, configPayload));

  daw::PluginCache pluginCache = daw::readPluginCache("plugin_cache.json");
  assert(!pluginCache.entries.empty());
  uint32_t pluginIndex = 0;
  for (size_t i = 0; i < pluginCache.entries.size(); ++i) {
    if (pluginCache.entries[i].isInstrument) {
      pluginIndex = static_cast<uint32_t>(i);
      break;
    }
  }
  daw::UiCommandPayload loadPayload{};
  loadPayload.commandType =
      static_cast<uint16_t>(daw::UiCommandType::LoadPluginOnTrack);
  loadPayload.trackId = 0;
  loadPayload.pluginIndex = pluginIndex;
  assert(sendUiCommand(ringUi, loadPayload));

  daw::UiCommandPayload playPayload{};
  playPayload.commandType =
      static_cast<uint16_t>(daw::UiCommandType::TogglePlay);
  playPayload.trackId = 0;
  assert(sendUiCommand(ringUi, playPayload));

  bool sawSnapshot = false;
  bool sawError = false;
  bool sawRouting = false;
  bool sawMod = false;
  bool sawModUid = false;
  bool sawPatcherDelta = false;
  bool sawVstInstrument = false;
  std::set<uint16_t> seenDiffTypes;
  daw::EventRingView ringUiOut =
      daw::makeEventRing(base, header->ringUiOutOffset);
  if (ringUiOut.mask != 0) {
    const auto start = std::chrono::steady_clock::now();
    daw::EventEntry diffEntry{};
    while (std::chrono::steady_clock::now() - start <
           std::chrono::milliseconds(500)) {
      if (!daw::ringPop(ringUiOut, diffEntry)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (diffEntry.type == static_cast<uint16_t>(daw::EventType::UiDiff)) {
        // Dispatch on diffType, which is what actually discriminates these payloads —
        // NOT on size. UiChainDiffPayload and UiChainErrorPayload are both 40 bytes, so
        // the size-based chain that used to be here routed every ChainError into the
        // ChainSnapshot branch and `sawError` could never be set. It went unnoticed
        // because RelWithDebInfo defines NDEBUG and compiled the assert away.
        uint16_t diffType = 0;
        std::memcpy(&diffType, diffEntry.payload, sizeof(diffType));
        // Record everything that arrived, so a failure below says WHICH diff was
        // missing and what came instead — a bare "sawMod is false" sends the reader
        // hunting through the engine with no idea whether the diff was absent, was an
        // error diff, or was decoded as the wrong type.
        seenDiffTypes.insert(diffType);
        switch (static_cast<daw::UiDiffType>(diffType)) {
          case daw::UiDiffType::ChainSnapshot: {
            daw::UiChainDiffPayload diff{};
            std::memcpy(&diff, diffEntry.payload, sizeof(diff));
            sawSnapshot = true;
            if (diff.deviceKind ==
                static_cast<uint32_t>(daw::DeviceKind::VstInstrument)) {
              sawVstInstrument = true;
            }
            break;
          }
          case daw::UiDiffType::ChainError:
            sawError = true;
            break;
          case daw::UiDiffType::RoutingSnapshot:
            sawRouting = true;
            break;
          case daw::UiDiffType::ModSnapshot:
            sawMod = true;
            break;
          case daw::UiDiffType::ModLinkUid16:
            sawModUid = true;
            break;
          case daw::UiDiffType::PatcherGraphDelta:
            sawPatcherDelta = true;
            break;
          default:
            break;
        }
      }
      if (sawSnapshot && sawError && sawRouting && sawMod && sawModUid &&
          sawPatcherDelta && sawVstInstrument) {
        break;
      }
    }
  }
  std::cout << "device_chain_ui: diff types seen =";
  for (uint16_t t : seenDiffTypes) {
    std::cout << " " << t;
  }
  std::cout << std::endl;
  assert(sawSnapshot);
  assert(sawError);
  assert(sawRouting);
  assert(sawMod);
  assert(sawModUid);
  assert(sawPatcherDelta);
  assert(sawVstInstrument);
  assert(waitForPlayheadAdvance(header, std::chrono::milliseconds(500)));

  int status = 0;
  ::waitpid(child, &status, 0);
  assert(WIFEXITED(status));
  assert(WEXITSTATUS(status) == 0);

  if (base && base != MAP_FAILED) {
    ::munmap(base, size);
  }
  if (fd >= 0) {
    ::close(fd);
  }
  ::shm_unlink(uiShmName.c_str());

  std::cout << "device_chain_ui_tests_main: ok" << std::endl;
  return 0;
}
