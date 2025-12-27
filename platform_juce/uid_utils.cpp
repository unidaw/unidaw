#include "platform_juce/uid_utils.h"

#include <algorithm>

#include <juce_cryptography/juce_cryptography.h>

namespace daw {

std::array<uint8_t, 16> md5Uid16FromIdentifier(const std::string& identifier) {
  const juce::MD5 md5(identifier.data(), identifier.size());
  std::array<uint8_t, 16> out{};
  const auto* bytes = md5.getChecksumDataArray();
  std::copy(bytes, bytes + out.size(), out.begin());
  return out;
}

std::string md5UidHexFromIdentifier(const std::string& identifier) {
  const juce::MD5 md5(identifier.data(), identifier.size());
  return md5.toHexString().toStdString();
}

}  // namespace daw
