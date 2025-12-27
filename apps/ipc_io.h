#pragma once

#include <cstddef>

#include "apps/ipc_protocol.h"

namespace daw {

bool readAll(int fd, void* buffer, size_t size);
bool writeAll(int fd, const void* buffer, size_t size);
bool sendMessage(int fd, ControlMessageType type, const void* payload, size_t size);
bool recvHeader(int fd, ControlHeader& header);

}  // namespace daw
