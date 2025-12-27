#include "apps/ipc_io.h"

#include <cstdint>
#include <unistd.h>

namespace daw {

bool readAll(int fd, void* buffer, size_t size) {
  auto* ptr = static_cast<uint8_t*>(buffer);
  size_t total = 0;
  while (total < size) {
    const ssize_t readBytes = ::read(fd, ptr + total, size - total);
    if (readBytes <= 0) {
      return false;
    }
    total += static_cast<size_t>(readBytes);
  }
  return true;
}

bool writeAll(int fd, const void* buffer, size_t size) {
  auto* ptr = static_cast<const uint8_t*>(buffer);
  size_t total = 0;
  while (total < size) {
    const ssize_t written = ::write(fd, ptr + total, size - total);
    if (written <= 0) {
      return false;
    }
    total += static_cast<size_t>(written);
  }
  return true;
}

bool sendMessage(int fd, ControlMessageType type, const void* payload, size_t size) {
  ControlHeader header;
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
