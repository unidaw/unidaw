#include "apps/host_controller.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>

#include "apps/ipc_io.h"

extern "C" char** environ;

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
  timeval timeout{};
  // Use longer timeout for plugin loading - complex plugins like Zebra2 can take 10+ seconds
  timeout.tv_sec = 60;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
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

SharedMemoryView::~SharedMemoryView() {
  if (base && base != MAP_FAILED) {
    ::munmap(base, size);
  }
}

HostController::~HostController() { disconnect(); }

bool HostController::launch(const HostConfig& config) {
  disconnect(); // Clean up any existing connection

  // Ensure old socket is gone so waitForSocket actually waits for the new one
  ::unlink(config.socketPath.c_str());

  std::cerr << "HostController: launching host (socket "
            << config.socketPath << ")" << std::endl;
  hostPid_ = spawnHostProcess(config);
  if (hostPid_ < 0) {
    return false;
  }

  int waitAttempts = 100;
  if (const char* env = std::getenv("DAW_HOST_SOCKET_WAIT_ATTEMPTS")) {
    char* end = nullptr;
    const long value = std::strtol(env, &end, 10);
    if (end != env && value > 0) {
      waitAttempts = static_cast<int>(value);
    }
  }
  std::cerr << "HostController: waiting for socket (" << waitAttempts
            << " attempts)" << std::endl;
  if (!waitForSocket(config.socketPath, waitAttempts)) {
    std::cerr << "HostController: waitForSocket(" << config.socketPath
              << ") timed out." << std::endl;
    killHostProcess();
    return false;
  }
  std::cerr << "HostController: socket ready, connecting" << std::endl;

  bool connected = false;
  for (int attempt = 0; attempt < waitAttempts; ++attempt) {
    if (connect(config)) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!connected) {
    killHostProcess();
    return false;
  }

  return true;
}

pid_t HostController::spawnHostProcess(const HostConfig& config) {
  // Default: ./juce_host_process relative to cwd. Because the string contains a
  // slash, posix_spawnp does NOT search PATH, so the engine otherwise only starts
  // from the build dir. DAW_HOST_BINARY overrides the path (absolute or relative)
  // so the engine can be launched from anywhere.
  std::string exe = "./juce_host_process";
  if (const char* env = std::getenv("DAW_HOST_BINARY")) {
    if (env[0] != '\0') {
      exe = env;
    }
  }

  std::vector<std::string> args;
  args.emplace_back(exe);
  args.emplace_back("--socket");
  args.emplace_back(config.socketPath);
  for (size_t i = 0; i < config.pluginPaths.size(); ++i) {
    const auto& path = config.pluginPaths[i];
    if (path.empty()) {
      continue;
    }
    args.emplace_back("--plugin");
    args.emplace_back(path);
    // Name-aware startup for multi-plugin bundles: pair each --plugin with the
    // desired sub-plugin name when we know it. Empty/absent => take the first type.
    if (i < config.pluginNames.size() && !config.pluginNames[i].empty()) {
      args.emplace_back("--plugin-name");
      args.emplace_back(config.pluginNames[i]);
    }
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  std::vector<std::string> env;
  if (!config.shmName.empty()) {
    env.emplace_back("DAW_SHM_NAME=" + config.shmName);
  }
  for (const auto& path : config.pluginPaths) {
    if (path.empty()) {
      continue;
    }
    const std::filesystem::path pluginPath(path);
    if (pluginPath.filename() == "Identity.vst3") {
      env.emplace_back("DAW_USE_FAKE_IDENTITY=1");
      break;
    }
  }
  std::vector<char*> envp;
  for (char** current = environ; *current != nullptr; ++current) {
    envp.push_back(*current);
  }
  for (auto& entry : env) {
    envp.push_back(const_cast<char*>(entry.c_str()));
  }
  envp.push_back(nullptr);

  pid_t pid = -1;
  const int spawnResult =
      ::posix_spawnp(&pid, exe.c_str(), nullptr, nullptr, argv.data(), envp.data());
  if (spawnResult != 0) {
    std::cerr << "posix_spawnp failed: " << std::strerror(spawnResult) << std::endl;
    return -1;
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

  std::cerr << "HostController: sending Hello" << std::endl;
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
    disconnectInternal(false);
    return false;
  }

  std::cerr << "HostController: waiting for Hello response" << std::endl;
  ControlHeader header;
  if (!recvHeader(socketFd_, header)) {
    std::cerr << "HostController: failed to receive Hello header." << std::endl;
    disconnectInternal(false);
    return false;
  }
  if (header.type != static_cast<uint16_t>(ControlMessageType::Hello) ||
      header.size != sizeof(HelloResponse)) {
    std::cerr << "HostController: invalid Hello response (type=" << header.type
              << " size=" << header.size << ")." << std::endl;
    disconnectInternal(false);
    return false;
  }

  HelloResponse response;
  if (!readAll(socketFd_, &response, sizeof(response))) {
    std::cerr << "HostController: failed to read Hello response." << std::endl;
    disconnectInternal(false);
    return false;
  }

  if (!mapSharedMemory(response, config)) {
    std::cerr << "HostController: failed to map shared memory." << std::endl;
    disconnectInternal(false);
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
  void* mapped = ::mmap(nullptr, shmSize_, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd_, 0);
  if (mapped == MAP_FAILED) {
    std::cerr << "HostController: mmap failed: " << std::strerror(errno) << std::endl;
    return false;
  }

  auto view = std::make_shared<SharedMemoryView>();
  view->base = mapped;
  view->size = shmSize_;
  view->header = reinterpret_cast<ShmHeader*>(mapped);
  view->mailbox = reinterpret_cast<BlockMailbox*>(
      reinterpret_cast<uint8_t*>(mapped) + view->header->mailboxOffset);
  view->completedBlockId = &view->mailbox->completedBlockId;

  shmView_ = view;
  shmBase_ = mapped;
  shmHeader_ = view->header;
  mailbox_ = view->mailbox;
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

  return true;
}

void HostController::disconnect() {
  disconnectInternal(true);
}

void HostController::disconnectInternal(bool killHost) {
  shmView_.reset();
  shmBase_ = nullptr;
  shmHeader_ = nullptr;
  mailbox_ = nullptr;
  shmSize_ = 0;
  closeFd(shmFd_);
  closeFd(socketFd_);
  // We assume that if we are disconnecting, we should also clean up the child process
  // if we launched it.
  if (killHost) {
    killHostProcess();
  }
}

bool HostController::sendProcessBlock(uint32_t blockId,
                                      uint64_t engineSampleStart,
                                      uint64_t pluginSampleStart,
                                      uint16_t segmentStart,
                                      uint16_t segmentLength,
                                      const HostTransport& transport) {
  ProcessBlockRequest request;
  request.blockId = blockId;
  request.engineSampleStart = engineSampleStart;
  request.pluginSampleStart = pluginSampleStart;
  request.segmentStart = segmentStart;
  request.segmentLength = segmentLength;
  request.flags = transport.isPlaying ? kProcessBlockFlagPlaying : 0u;
  request.bpm = transport.bpm;
  request.ppqPosition = transport.ppqPosition;
  request.ppqPositionOfLastBarStart = transport.ppqPositionOfLastBarStart;
  request.timeSigNumerator = transport.timeSigNumerator;
  request.timeSigDenominator = transport.timeSigDenominator;
  std::lock_guard<std::mutex> lock(socketMutex_);
  return sendMessageNonBlocking(socketFd_,
                                ControlMessageType::ProcessBlock,
                                &request,
                                sizeof(request));
}

bool HostController::requestPluginState(uint32_t pluginIndex,
                                        std::vector<uint8_t>& out) {
  out.clear();
  std::lock_guard<std::mutex> lock(socketMutex_);
  if (socketFd_ < 0) {
    return false;
  }
  StateHeader request{};
  request.pluginIndex = pluginIndex;
  request.byteCount = 0;
  if (!sendMessage(socketFd_, ControlMessageType::GetState, &request, sizeof(request))) {
    return false;
  }

  ControlHeader header;
  if (!recvHeader(socketFd_, header)) {
    return false;
  }
  if (header.type != static_cast<uint16_t>(ControlMessageType::GetState) ||
      header.size < sizeof(StateHeader)) {
    return false;
  }
  std::vector<uint8_t> payload(header.size);
  if (!readAll(socketFd_, payload.data(), payload.size())) {
    return false;
  }
  StateHeader response{};
  std::memcpy(&response, payload.data(), sizeof(response));
  const size_t available = payload.size() - sizeof(StateHeader);
  const size_t count = std::min<size_t>(response.byteCount, available);
  out.assign(payload.begin() + sizeof(StateHeader),
             payload.begin() + sizeof(StateHeader) + count);
  return true;
}

bool HostController::requestPluginParams(uint32_t pluginIndex,
                                         std::vector<HostParamWire>& out,
                                         std::string& outPluginName) {
  out.clear();
  outPluginName.clear();
  std::lock_guard<std::mutex> lock(socketMutex_);
  if (socketFd_ < 0) {
    return false;
  }
  ParamsHeader request{};
  request.pluginIndex = pluginIndex;
  if (!sendMessage(socketFd_, ControlMessageType::GetParams, &request,
                   sizeof(request))) {
    return false;
  }
  ControlHeader header;
  if (!recvHeader(socketFd_, header)) {
    return false;
  }
  if (header.type != static_cast<uint16_t>(ControlMessageType::GetParams) ||
      header.size < sizeof(ParamsHeader)) {
    return false;
  }
  std::vector<uint8_t> payload(header.size);
  if (!readAll(socketFd_, payload.data(), payload.size())) {
    return false;
  }
  ParamsHeader response{};
  std::memcpy(&response, payload.data(), sizeof(response));
  outPluginName.assign(response.pluginName,
                       ::strnlen(response.pluginName, sizeof(response.pluginName)));
  const size_t available = payload.size() - sizeof(ParamsHeader);
  const uint32_t count = std::min<uint32_t>(response.paramCount, kMaxParamsPerQuery);
  const size_t needed = static_cast<size_t>(count) * sizeof(HostParamWire);
  if (needed > available) {
    return false;
  }
  out.resize(count);
  if (count > 0) {
    std::memcpy(out.data(), payload.data() + sizeof(ParamsHeader), needed);
  }
  return true;
}

bool HostController::requestBusLayout(uint32_t pluginIndex,
                                      std::vector<HostBusWire>& out,
                                      bool& outTruncated) {
  out.clear();
  outTruncated = false;
  std::lock_guard<std::mutex> lock(socketMutex_);
  if (socketFd_ < 0) {
    return false;
  }
  BusLayoutHeader request{};
  request.pluginIndex = pluginIndex;
  if (!sendMessage(socketFd_, ControlMessageType::GetBusLayout, &request,
                   sizeof(request))) {
    return false;
  }
  ControlHeader header;
  if (!recvHeader(socketFd_, header)) {
    return false;
  }
  if (header.type != static_cast<uint16_t>(ControlMessageType::GetBusLayout) ||
      header.size < sizeof(BusLayoutHeader)) {
    return false;
  }
  std::vector<uint8_t> payload(header.size);
  if (!readAll(socketFd_, payload.data(), payload.size())) {
    return false;
  }
  BusLayoutHeader response{};
  std::memcpy(&response, payload.data(), sizeof(response));
  outTruncated = response.truncated != 0;
  const size_t available = payload.size() - sizeof(BusLayoutHeader);
  const uint32_t count = std::min<uint32_t>(response.busCount, kMaxBusesPerQuery);
  const size_t needed = static_cast<size_t>(count) * sizeof(HostBusWire);
  if (needed > available) {
    return false;
  }
  out.resize(count);
  if (count > 0) {
    std::memcpy(out.data(), payload.data() + sizeof(BusLayoutHeader), needed);
  }
  return true;
}

bool HostController::requestChainLatency(uint32_t& outTotalSamples,
                                         std::vector<int32_t>& outPerPlugin) {
  outTotalSamples = 0;
  outPerPlugin.clear();
  std::lock_guard<std::mutex> lock(socketMutex_);
  if (socketFd_ < 0) {
    return false;
  }
  // Send a LatencyHeader as the request body — its contents are ignored host-side, but
  // the body must be non-empty: the host's control loop only dispatches a message when
  // header.size > 0, so a zero-length request would be silently skipped and this call
  // would block forever waiting for a reply that never comes.
  LatencyHeader request{};
  if (!sendMessage(socketFd_, ControlMessageType::GetLatency, &request,
                   sizeof(request))) {
    return false;
  }
  ControlHeader header;
  if (!recvHeader(socketFd_, header)) {
    return false;
  }
  if (header.type != static_cast<uint16_t>(ControlMessageType::GetLatency) ||
      header.size < sizeof(LatencyHeader)) {
    return false;
  }
  std::vector<uint8_t> payload(header.size);
  if (!readAll(socketFd_, payload.data(), payload.size())) {
    return false;
  }
  LatencyHeader response{};
  std::memcpy(&response, payload.data(), sizeof(response));
  outTotalSamples = response.totalSamples;
  const size_t available = payload.size() - sizeof(LatencyHeader);
  const uint32_t count = std::min<uint32_t>(response.pluginCount, kMaxParamsPerQuery);
  const size_t needed = static_cast<size_t>(count) * sizeof(int32_t);
  if (needed > available) {
    return false;
  }
  outPerPlugin.resize(count);
  if (count > 0) {
    std::memcpy(outPerPlugin.data(), payload.data() + sizeof(LatencyHeader), needed);
  }
  return true;
}

bool HostController::sendPluginState(uint32_t pluginIndex,
                                     const std::vector<uint8_t>& data) {
  std::lock_guard<std::mutex> lock(socketMutex_);
  if (socketFd_ < 0) {
    return false;
  }
  std::vector<uint8_t> payload(sizeof(StateHeader) + data.size());
  StateHeader header{};
  header.pluginIndex = pluginIndex;
  header.byteCount = static_cast<uint32_t>(data.size());
  std::memcpy(payload.data(), &header, sizeof(header));
  if (!data.empty()) {
    std::memcpy(payload.data() + sizeof(header), data.data(), data.size());
  }
  return sendMessage(socketFd_,
                     ControlMessageType::SetState,
                     payload.data(),
                     payload.size());
}

bool HostController::sendSetChain(const std::vector<PluginRef>& refs,
                                  uint32_t sidechainMask) {
  // Each entry is path\0name\0 (v4). The name lets the host pick the right plugin
  // out of a multi-plugin bundle; an empty name means "take the first type".
  std::vector<uint8_t> block;
  for (const auto& ref : refs) {
    block.insert(block.end(), ref.path.begin(), ref.path.end());
    block.push_back('\0');
    block.insert(block.end(), ref.name.begin(), ref.name.end());
    block.push_back('\0');
  }
  std::vector<uint8_t> payload(sizeof(ChainHeader) + block.size());
  ChainHeader header{};
  header.count = static_cast<uint32_t>(refs.size());
  header.byteCount = static_cast<uint32_t>(block.size());
  header.sidechainMask = sidechainMask;
  std::memcpy(payload.data(), &header, sizeof(header));
  if (!block.empty()) {
    std::memcpy(payload.data() + sizeof(header), block.data(), block.size());
  }
  std::lock_guard<std::mutex> lock(socketMutex_);
  if (socketFd_ < 0) {
    return false;
  }
  return sendMessage(socketFd_, ControlMessageType::SetChain,
                     payload.data(), payload.size());
}

bool HostController::sendOpenEditor(uint32_t pluginIndex) {
  OpenEditorRequest request;
  request.pluginIndex = pluginIndex;
  std::lock_guard<std::mutex> lock(socketMutex_);
  return sendMessage(socketFd_, ControlMessageType::OpenEditor, &request, sizeof(request));
}

bool HostController::sendSetBypass(uint32_t pluginIndex, bool bypass) {
  SetBypassRequest request;
  request.pluginIndex = pluginIndex;
  request.bypass = bypass ? 1u : 0u;
  std::lock_guard<std::mutex> lock(socketMutex_);
  return sendMessage(socketFd_, ControlMessageType::SetBypass, &request, sizeof(request));
}

bool HostController::sendSetParam(uint32_t pluginIndex, const uint8_t* uid16,
                                 float normalized) {
  SetParamRequest request;
  request.pluginIndex = pluginIndex;
  std::memcpy(request.uid16, uid16, sizeof(request.uid16));
  request.normalized = normalized;
  std::lock_guard<std::mutex> lock(socketMutex_);
  if (socketFd_ < 0) {
    return false;
  }
  return sendMessage(socketFd_, ControlMessageType::SetParam, &request, sizeof(request));
}

bool HostController::sendShutdown() {
  std::lock_guard<std::mutex> lock(socketMutex_);
  return sendMessage(socketFd_, ControlMessageType::Shutdown, nullptr, 0);
}

}  // namespace daw
