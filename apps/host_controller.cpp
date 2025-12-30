#include "apps/host_controller.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include "apps/ipc_io.h"

namespace daw {
namespace {

void closeFd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

bool connectSocket(int& fd, const std::string& path) {
  fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "HostController: socket() failed: " << std::strerror(errno) << std::endl;
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "HostController: connect(" << path
              << ") failed: " << std::strerror(errno) << std::endl;
    closeFd(fd);
    return false;
  }
  return true;
}

}  // namespace

HostController::~HostController() { disconnect(); }

bool HostController::launch(const HostConfig& config) {
  disconnect(); // Clean up any existing connection

  // Ensure old socket is gone so waitForSocket actually waits for the new one
  ::unlink(config.socketPath.c_str());

  hostPid_ = spawnHostProcess(config);
  if (hostPid_ < 0) {
    return false;
  }

  if (!waitForSocket(config.socketPath, 100)) {
    std::cerr << "HostController: waitForSocket(" << config.socketPath
              << ") timed out." << std::endl;
    killHostProcess();
    return false;
  }

  if (!connect(config)) {
    killHostProcess();
    return false;
  }

  return true;
}

pid_t HostController::spawnHostProcess(const HostConfig& config) {
  pid_t pid = ::fork();
  if (pid == 0) {
    // Child process
    // We assume the executable is in the current directory or PATH.
    // For this environment, we know it's ./juce_host_process in build dir usually,
    // but let's try to be relative to current dir.
    std::string exe = "./juce_host_process"; 
    
    if (!config.shmName.empty()) {
      ::setenv("DAW_SHM_NAME", config.shmName.c_str(), 1);
    }
    if (!config.pluginPath.empty()) {
      ::execl(exe.c_str(), exe.c_str(), "--socket",
              config.socketPath.c_str(), "--plugin", config.pluginPath.c_str(), nullptr);
    } else {
      ::execl(exe.c_str(), exe.c_str(), "--socket",
              config.socketPath.c_str(), nullptr);
    }
    // If execl fails:
    std::perror("execl");
    std::_Exit(127);
  }
  return pid;
}

void HostController::killHostProcess() {
  if (hostPid_ > 0) {
    ::kill(hostPid_, SIGKILL); // Hard kill per watchdog spec
    ::waitpid(hostPid_, nullptr, 0);
    hostPid_ = -1;
  }
}

bool HostController::waitForSocket(const std::string& path, int attempts) {
  for (int i = 0; i < attempts; ++i) {
    if (::access(path.c_str(), F_OK) == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}


bool HostController::connect(const HostConfig& config) {
  if (!connectSocket(socketFd_, config.socketPath)) {
    return false;
  }

  HelloRequest request;
  request.blockSize = config.blockSize;
  request.numChannelsIn = config.numChannelsIn;
  request.numChannelsOut = config.numChannelsOut;
  request.numBlocks = config.numBlocks;
  request.ringStdCapacity = config.ringStdCapacity;
  request.ringCtrlCapacity = config.ringCtrlCapacity;
  request.ringUiCapacity = config.ringUiCapacity;
  request.sampleRate = config.sampleRate;

  if (!sendMessage(socketFd_, ControlMessageType::Hello, &request, sizeof(request))) {
    std::cerr << "HostController: failed to send Hello." << std::endl;
    disconnect();
    return false;
  }

  ControlHeader header;
  if (!recvHeader(socketFd_, header)) {
    std::cerr << "HostController: failed to receive Hello header." << std::endl;
    disconnect();
    return false;
  }
  if (header.type != static_cast<uint16_t>(ControlMessageType::Hello) ||
      header.size != sizeof(HelloResponse)) {
    std::cerr << "HostController: invalid Hello response (type=" << header.type
              << " size=" << header.size << ")." << std::endl;
    disconnect();
    return false;
  }

  HelloResponse response;
  if (!readAll(socketFd_, &response, sizeof(response))) {
    std::cerr << "HostController: failed to read Hello response." << std::endl;
    disconnect();
    return false;
  }

  if (!mapSharedMemory(response, config)) {
    std::cerr << "HostController: failed to map shared memory." << std::endl;
    disconnect();
    return false;
  }

  return true;
}

bool HostController::mapSharedMemory(const HelloResponse& response,
                                     const HostConfig& config) {
  shmFd_ = ::shm_open(response.shmName, O_RDWR, 0600);
  if (shmFd_ < 0) {
    std::cerr << "HostController: shm_open(" << response.shmName
              << ") failed: " << std::strerror(errno) << std::endl;
    return false;
  }

  shmSize_ = static_cast<size_t>(response.shmSizeBytes);
  shmBase_ = ::mmap(nullptr, shmSize_, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd_, 0);
  if (shmBase_ == MAP_FAILED) {
    shmBase_ = nullptr;
    std::cerr << "HostController: mmap failed: " << std::strerror(errno) << std::endl;
    return false;
  }

  shmHeader_ = reinterpret_cast<ShmHeader*>(shmBase_);
  if (shmHeader_->magic != kShmMagic || shmHeader_->version != kShmVersion) {
    std::cerr << "HostController: shm header mismatch (magic="
              << std::hex << shmHeader_->magic << " version=" << std::dec
              << shmHeader_->version << ")." << std::endl;
    return false;
  }
  if (shmHeader_->blockSize != config.blockSize ||
      shmHeader_->sampleRate != config.sampleRate ||
      shmHeader_->numChannelsIn != config.numChannelsIn ||
      shmHeader_->numChannelsOut != config.numChannelsOut ||
      shmHeader_->numBlocks != config.numBlocks) {
    std::cerr << "HostController: shm config mismatch (blockSize="
              << shmHeader_->blockSize << " sampleRate=" << shmHeader_->sampleRate
              << " numChannelsIn=" << shmHeader_->numChannelsIn
              << " numChannelsOut=" << shmHeader_->numChannelsOut
              << " numBlocks=" << shmHeader_->numBlocks << ")." << std::endl;
    return false;
  }

  mailbox_ = reinterpret_cast<BlockMailbox*>(
      reinterpret_cast<uint8_t*>(shmBase_) + shmHeader_->mailboxOffset);
  return true;
}

void HostController::disconnect() {
  if (shmBase_ && shmBase_ != MAP_FAILED) {
    ::munmap(shmBase_, shmSize_);
    shmBase_ = nullptr;
  }
  closeFd(shmFd_);
  closeFd(socketFd_);
  // We assume that if we are disconnecting, we should also clean up the child process
  // if we launched it.
  killHostProcess();
}

bool HostController::sendProcessBlock(uint32_t blockId,
                                      uint64_t engineSampleStart,
                                      uint64_t pluginSampleStart) {
  ProcessBlockRequest request;
  request.blockId = blockId;
  request.engineSampleStart = engineSampleStart;
  request.pluginSampleStart = pluginSampleStart;
  return sendMessage(socketFd_, ControlMessageType::ProcessBlock, &request, sizeof(request));
}

bool HostController::sendShutdown() {
  return sendMessage(socketFd_, ControlMessageType::Shutdown, nullptr, 0);
}

}  // namespace daw
