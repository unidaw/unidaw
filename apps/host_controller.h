#pragma once

#include <sys/types.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "apps/ipc_protocol.h"
#include "apps/shared_memory.h"

namespace daw {

struct HostConfig {
  std::string socketPath;
  std::vector<std::string> pluginPaths;
  std::string shmName;
  uint32_t blockSize = 512;
  double sampleRate = 48000.0;
  uint32_t numChannelsIn = 0;
  uint32_t numChannelsOut = 2;
  uint32_t numBlocks = 3;
  uint32_t ringStdCapacity = 1024;
  uint32_t ringCtrlCapacity = 128;
  uint32_t ringUiCapacity = 128;
};

struct SharedMemoryView {
  void* base = nullptr;
  size_t size = 0;
  ShmHeader* header = nullptr;
  BlockMailbox* mailbox = nullptr;
  std::atomic<uint32_t>* completedBlockId = nullptr;
  ~SharedMemoryView();
};

// Musical position for a block, forwarded to the hosted plugin's play head.
struct HostTransport {
  double bpm = 120.0;
  double ppqPosition = 0.0;
  double ppqPositionOfLastBarStart = 0.0;
  uint32_t timeSigNumerator = 4;
  uint32_t timeSigDenominator = 4;
  bool isPlaying = false;
};

class HostController {
 public:
  HostController() = default;
  ~HostController();

  // Spawns and connects. Returns true on success.
  bool launch(const HostConfig& config);
  
  // Just connects (if already spawned externally or checking manually)
  bool connect(const HostConfig& config);
  
  void disconnect();
  bool sendProcessBlock(uint32_t blockId,
                        uint64_t engineSampleStart,
                        uint64_t pluginSampleStart,
                        uint16_t segmentStart = 0,
                        uint16_t segmentLength = 0,
                        const HostTransport& transport = HostTransport{});
  // Blocking request/response over the control socket. Called from the UI
  // thread during save; the producer thread may briefly wait on the socket
  // mutex, which is acceptable for an explicit user action.
  bool requestPluginState(uint32_t pluginIndex, std::vector<uint8_t>& out);
  bool sendPluginState(uint32_t pluginIndex, const std::vector<uint8_t>& data);

  bool sendOpenEditor(uint32_t pluginIndex);
  bool sendSetBypass(uint32_t pluginIndex, bool bypass);
  bool sendShutdown();
  pid_t hostPid() const { return hostPid_; }

  const ShmHeader* shmHeader() const { return shmHeader_; }
  const BlockMailbox* mailbox() const { return mailbox_; }
  size_t shmSize() const { return shmSize_; }
  std::shared_ptr<const SharedMemoryView> sharedMemory() const { return shmView_; }

 private:
 bool mapSharedMemory(const HelloResponse& response, const HostConfig& config);
  pid_t spawnHostProcess(const HostConfig& config);
  void killHostProcess();
  bool waitForSocket(const std::string& path, int attempts);
  void disconnectInternal(bool killHost);

  // Serialises writes to the control socket. Both the render-ahead producer
  // thread (ProcessBlock) and the UI thread (editor, bypass, state) write to
  // it, and a request that expects a reply must not interleave with anything.
  std::mutex socketMutex_;
  int socketFd_ = -1;
  int shmFd_ = -1;
  void* shmBase_ = nullptr;
  size_t shmSize_ = 0;
  ShmHeader* shmHeader_ = nullptr;
  BlockMailbox* mailbox_ = nullptr;
  std::shared_ptr<SharedMemoryView> shmView_;
  pid_t hostPid_ = -1;
};

}  // namespace daw
