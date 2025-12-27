#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace daw {

std::string md5UidHexFromIdentifier(const std::string& identifier);
std::array<uint8_t, 16> md5Uid16FromIdentifier(const std::string& identifier);

}  // namespace daw
