#pragma once

#include <sys/types.h>
#include <cstdint>
#include <string>

#include "apps/ipc_protocol.h"
#include "apps/shared_memory.h"

namespace daw {

struct HostConfig {
  std::string socketPath;
  std::string pluginPath; // Added plugin path
  uint32_t blockSize = 512;
  double sampleRate = 48000.0;
  uint32_t numChannelsIn = 0;
  uint32_t numChannelsOut = 2;
  uint32_t numBlocks = 3;
  uint32_t ringStdCapacity = 1024;
  uint32_t ringCtrlCapacity = 128;
  uint32_t ringUiCapacity = 128;
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
                        uint64_t pluginSampleStart);
  bool sendShutdown();
  pid_t hostPid() const { return hostPid_; }

  const ShmHeader* shmHeader() const { return shmHeader_; }
  const BlockMailbox* mailbox() const { return mailbox_; }

 private:
  bool mapSharedMemory(const HelloResponse& response, const HostConfig& config);
  pid_t spawnHostProcess(const std::string& socketPath, const std::string& pluginPath);
  void killHostProcess();
  bool waitForSocket(const std::string& path, int attempts);

  int socketFd_ = -1;
  int shmFd_ = -1;
  void* shmBase_ = nullptr;
  size_t shmSize_ = 0;
  ShmHeader* shmHeader_ = nullptr;
  BlockMailbox* mailbox_ = nullptr;
  pid_t hostPid_ = -1;
};

}  // namespace daw
