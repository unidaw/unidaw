#include "apps/ipc_io.h"

#include <cstdint>
#include <array>
#include <cerrno>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/socket.h>

namespace daw {

bool readAll(int fd, void* buffer, size_t size) {
  auto* ptr = static_cast<uint8_t*>(buffer);
  size_t total = 0;
  int eagainAttempts = 0;
  constexpr int kMaxEagainAttempts = 4;
  while (total < size) {
    const ssize_t readBytes = ::read(fd, ptr + total, size - total);
    if (readBytes > 0) {
      total += static_cast<size_t>(readBytes);
      eagainAttempts = 0;
      continue;
    }
    if (readBytes == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (eagainAttempts++ < kMaxEagainAttempts) {
        std::this_thread::yield();
        continue;
      }
      return false;
    }
    return false;
  }
  return true;
}

bool writeAll(int fd, const void* buffer, size_t size) {
  auto* ptr = static_cast<const uint8_t*>(buffer);
  size_t total = 0;
  int eagainAttempts = 0;
  constexpr int kMaxEagainAttempts = 4;
  while (total < size) {
    const ssize_t written = ::write(fd, ptr + total, size - total);
    if (written > 0) {
      total += static_cast<size_t>(written);
      eagainAttempts = 0;
      continue;
    }
    if (written == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (eagainAttempts++ < kMaxEagainAttempts) {
        std::this_thread::yield();
        continue;
      }
      return false;
    }
    return false;
  }
  return true;
}

bool sendMessage(int fd, ControlMessageType type, const void* payload, size_t size) {
  ControlHeader header{};
  header.magic = kControlMagic;
  header.version = kControlVersion;
  header.type = static_cast<uint16_t>(type);
  header.size = static_cast<uint32_t>(size);
  if (!writeAll(fd, &header, sizeof(header))) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  return writeAll(fd, payload, size);
}

bool sendMessageNonBlocking(int fd,
                            ControlMessageType type,
                            const void* payload,
                            size_t size) {
  ControlHeader header{};
  header.magic = kControlMagic;
  header.version = kControlVersion;
  header.type = static_cast<uint16_t>(type);
  header.size = static_cast<uint32_t>(size);

  constexpr size_t kMaxPayload = 128;
  if (size > kMaxPayload) {
    return false;
  }

  std::array<uint8_t, sizeof(ControlHeader) + kMaxPayload> buffer{};
  std::memcpy(buffer.data(), &header, sizeof(header));
  if (size > 0 && payload) {
    std::memcpy(buffer.data() + sizeof(header), payload, size);
  }
  const size_t total = sizeof(header) + size;
  int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  size_t sentTotal = 0;
  int eagainAttempts = 0;
  constexpr int kMaxEagainAttempts = 20;
  constexpr int kPollTimeoutMs = 2;
  constexpr int kMaxPolls = 50;
  int polls = 0;
  while (sentTotal < total) {
    const ssize_t sent = ::send(fd,
                                buffer.data() + sentTotal,
                                total - sentTotal,
                                flags);
    if (sent > 0) {
      sentTotal += static_cast<size_t>(sent);
      continue;
    }
    if (sent == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (polls < kMaxPolls) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        const int ready = ::poll(&pfd, 1, kPollTimeoutMs);
        ++polls;
        if (ready > 0 && (pfd.revents & POLLOUT)) {
          continue;
        }
      }
      if (eagainAttempts++ < kMaxEagainAttempts) {
        std::this_thread::yield();
        continue;
      }
      return false;
    }
    return false;
  }
  return true;
}

bool recvHeader(int fd, ControlHeader& header) {
  if (!readAll(fd, &header, sizeof(header))) {
    return false;
  }
  if (header.magic != kControlMagic || header.version != kControlVersion) {
    return false;
  }
  return true;
}

}  // namespace daw
