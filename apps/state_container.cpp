#include "apps/state_container.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace daw {
namespace {

uint32_t parseVersionNumber(const std::string& version) {
  uint32_t value = 0;
  bool hasDigits = false;
  for (const char c : version) {
    if (c >= '0' && c <= '9') {
      value = value * 10 + static_cast<uint32_t>(c - '0');
      hasDigits = true;
    } else if (hasDigits) {
      break;
    }
  }
  return hasDigits ? value : 0u;
}

void writeLe16(std::ostream& out, uint16_t value) {
  const uint8_t bytes[2] = {
      static_cast<uint8_t>(value & 0xFFu),
      static_cast<uint8_t>((value >> 8) & 0xFFu),
  };
  out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void writeLe32(std::ostream& out, uint32_t value) {
  const uint8_t bytes[4] = {
      static_cast<uint8_t>(value & 0xFFu),
      static_cast<uint8_t>((value >> 8) & 0xFFu),
      static_cast<uint8_t>((value >> 16) & 0xFFu),
      static_cast<uint8_t>((value >> 24) & 0xFFu),
  };
  out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

bool readLe16(std::istream& in, uint16_t& value) {
  uint8_t bytes[2];
  in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
  if (!in) {
    return false;
  }
  value = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
  return true;
}

bool readLe32(std::istream& in, uint32_t& value) {
  uint8_t bytes[4];
  in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
  if (!in) {
    return false;
  }
  value = static_cast<uint32_t>(bytes[0]) |
          (static_cast<uint32_t>(bytes[1]) << 8) |
          (static_cast<uint32_t>(bytes[2]) << 16) |
          (static_cast<uint32_t>(bytes[3]) << 24);
  return true;
}

bool fsyncFileDescriptor(int fd) {
  if (fd < 0) {
    return false;
  }
  return ::fsync(fd) == 0;
}

bool fsyncPath(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  const bool ok = fsyncFileDescriptor(fd);
  if (fd >= 0) {
    ::close(fd);
  }
  return ok;
}

}  // namespace

uint32_t crc32cUpdate(uint32_t crc, const uint8_t* data, size_t size) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t r = i;
      for (int j = 0; j < 8; ++j) {
        r = (r >> 1) ^ (0x82F63B78u & (~(r & 1u) + 1u));
      }
      table[i] = r;
    }
    init = true;
  }

  uint32_t c = ~crc;
  for (size_t i = 0; i < size; ++i) {
    c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return ~c;
}

uint32_t crc32c(const uint8_t* data, size_t size) {
  return crc32cUpdate(0, data, size);
}

bool writeStateFile(const std::string& path,
                    const std::string& version,
                    const std::array<uint8_t, 16>& uid16,
                    const std::vector<uint8_t>& state) {
  if (state.empty()) {
    return false;
  }

  const uint32_t payloadCrc = crc32c(state.data(), state.size());
  PluginStateHeader header;
  header.headerSize = static_cast<uint16_t>(sizeof(PluginStateHeader));
  std::copy(uid16.begin(), uid16.end(), header.uid16);
  header.pluginVersion = parseVersionNumber(version);
  header.payloadSize = static_cast<uint32_t>(state.size());
  header.payloadCrc32c = payloadCrc;

  PluginStateHeader headerForCrc = header;
  headerForCrc.headerCrc32c = 0;
  header.headerCrc32c =
      crc32c(reinterpret_cast<const uint8_t*>(&headerForCrc), sizeof(headerForCrc));

  const std::string tmpPath = path + ".tmp";
  std::ofstream out(tmpPath, std::ios::binary);
  if (!out) {
    return false;
  }

  out.write(header.magic, sizeof(header.magic));
  writeLe16(out, header.version);
  writeLe16(out, header.headerSize);
  out.write(reinterpret_cast<const char*>(header.uid16), sizeof(header.uid16));
  writeLe32(out, header.pluginVersion);
  writeLe16(out, header.stateFormat);
  writeLe16(out, header.flags);
  writeLe32(out, header.payloadSize);
  writeLe32(out, header.payloadCrc32c);
  writeLe32(out, header.headerCrc32c);
  out.write(reinterpret_cast<const char*>(state.data()),
            static_cast<std::streamsize>(state.size()));
  out.flush();
  if (!out) {
    return false;
  }
  out.close();

  const int fd = ::open(tmpPath.c_str(), O_RDONLY);
  const bool dataSynced = fsyncFileDescriptor(fd);
  if (fd >= 0) {
    ::close(fd);
  }
  if (!dataSynced) {
    return false;
  }
  if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
    return false;
  }
  const auto dirPos = path.find_last_of('/');
  if (dirPos != std::string::npos) {
    fsyncPath(path.substr(0, dirPos));
  }
  return true;
}

bool readStateFile(const std::string& path,
                   std::string& version,
                   std::array<uint8_t, 16>& uid16,
                   std::vector<uint8_t>& state) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }

  PluginStateHeader header;
  in.read(header.magic, sizeof(header.magic));
  if (!in || std::string(header.magic, header.magic + 4) != "PST0") {
    return false;
  }
  if (!readLe16(in, header.version) || !readLe16(in, header.headerSize)) {
    return false;
  }
  if (header.version != 1 || header.headerSize != sizeof(PluginStateHeader)) {
    return false;
  }
  in.read(reinterpret_cast<char*>(header.uid16), sizeof(header.uid16));
  if (!readLe32(in, header.pluginVersion) ||
      !readLe16(in, header.stateFormat) ||
      !readLe16(in, header.flags) ||
      !readLe32(in, header.payloadSize) ||
      !readLe32(in, header.payloadCrc32c) ||
      !readLe32(in, header.headerCrc32c)) {
    return false;
  }
  if (!in) {
    return false;
  }

  PluginStateHeader headerForCrc = header;
  headerForCrc.headerCrc32c = 0;
  const uint32_t headerCrc =
      crc32c(reinterpret_cast<const uint8_t*>(&headerForCrc), sizeof(headerForCrc));
  if (headerCrc != header.headerCrc32c) {
    return false;
  }

  uid16.fill(0);
  std::copy(std::begin(header.uid16), std::end(header.uid16), uid16.begin());
  version = std::to_string(header.pluginVersion);
  state.resize(header.payloadSize);
  if (!state.empty()) {
    in.read(reinterpret_cast<char*>(state.data()),
            static_cast<std::streamsize>(state.size()));
  }

  if (!in) {
    return false;
  }

  const uint32_t checksum = crc32c(state.data(), state.size());
  if (checksum != header.payloadCrc32c) {
    return false;
  }

  return true;
}

}  // namespace daw
