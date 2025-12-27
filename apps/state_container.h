#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

struct PluginStateHeader {
  char magic[4] = {'P', 'S', 'T', '0'};
  uint16_t version = 1;
  uint16_t headerSize = 0;
  uint8_t uid16[16]{};
  uint32_t pluginVersion = 0;
  uint16_t stateFormat = 0;
  uint16_t flags = 0;
  uint32_t payloadSize = 0;
  uint32_t payloadCrc32c = 0;
  uint32_t headerCrc32c = 0;
};

uint32_t crc32cUpdate(uint32_t crc, const uint8_t* data, size_t size);
uint32_t crc32c(const uint8_t* data, size_t size);

bool writeStateFile(const std::string& path,
                    const std::string& version,
                    const std::array<uint8_t, 16>& uid16,
                    const std::vector<uint8_t>& state);

bool readStateFile(const std::string& path,
                   std::string& version,
                   std::array<uint8_t, 16>& uid16,
                   std::vector<uint8_t>& state);

}  // namespace daw
