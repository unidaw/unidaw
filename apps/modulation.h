#pragma once

#include <cstdint>
#include <vector>

namespace daw {

enum class ModRate : uint8_t {
  BlockRate = 0,
  SampleRate = 1,
};

constexpr uint32_t kModLinkIdAuto = 0xFFFFFFFFu;

enum class ModSourceKind : uint8_t {
  Macro = 0,
  Lfo = 1,
  Envelope = 2,
  PatcherNodeOutput = 3,
};

enum class ModTargetKind : uint8_t {
  VstParam = 0,
  PatcherParam = 1,
  PatcherMacro = 2,
};

struct ModSourceRef {
  uint32_t deviceId = 0;
  uint32_t sourceId = 0;
  ModSourceKind kind = ModSourceKind::Macro;

  // A plain value. The document walk reaches these through ModLink and asks whether the
  // user changed anything; the compiler writes the comparison.
  friend bool operator==(const ModSourceRef&, const ModSourceRef&) = default;
};

struct ModTargetRef {
  uint32_t deviceId = 0;
  uint32_t targetId = 0;
  ModTargetKind kind = ModTargetKind::VstParam;
  uint8_t uid16[16]{};

  // A plain value. The document walk reaches these through ModLink and asks whether the
  // user changed anything; the compiler writes the comparison.
  friend bool operator==(const ModTargetRef&, const ModTargetRef&) = default;
};

struct ModSourceState {
  ModSourceRef ref{};
  float value = 0.0f;
};

struct ModLink {
  uint32_t linkId = 0;
  ModSourceRef source{};
  ModTargetRef target{};
  float depth = 0.0f;
  float bias = 0.0f;
  ModRate rate = ModRate::BlockRate;
  bool enabled = true;
};

struct ModRegistry {
  std::vector<ModSourceState> sources;
  std::vector<ModLink> links;
};

}  // namespace daw
