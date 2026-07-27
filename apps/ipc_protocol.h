#pragma once

#include <cstdint>
#include <string>

namespace daw {

// One plugin in a chain: the bundle path plus the desired plugin name within it. A
// VST3 bundle can hold several plugins (Zebra2.vst3 = Zebra2/Zebralette/ZRev/Zebrify)
// and the path alone cannot say which; `name` disambiguates. Empty name = take the
// first type (correct for a single-plugin bundle). Carried over SetChain as
// path\0name\0 pairs (kControlVersion 4).
struct PluginRef {
  std::string path;
  std::string name;
};

constexpr uint32_t kControlMagic = 0x30485744;  // 'DWH0'
// 2: ProcessBlockRequest carries transport position for the play head.
// 3: GetParams + ParamsHeader.pluginName (B1). Bumped so a host built before this
//    is rejected at the handshake (recvHeader checks the version) instead of
//    misparsing the larger reply header into a silent empty param list.
// 4: SetChain entries are now path\0name\0 pairs (not bare paths) so the host can
//    pick the right sub-plugin out of a multi-plugin VST3 bundle (Zebra2.vst3 holds
//    Zebra2/Zebralette/ZRev/Zebrify). An older host would read the name as a second
//    path and load garbage, so the version gate must reject it.
constexpr uint16_t kControlVersion = 4;

enum class ControlMessageType : uint16_t {
  Hello = 1,
  ProcessBlock = 2,
  Shutdown = 3,
  OpenEditor = 4,
  SetBypass = 5,
  // Opaque plugin state. Payload is StateHeader followed by the blob; the
  // reply to GetState reuses the same shape. Without these a chain edit
  // restarts the host and the sound is gone.
  GetState = 6,
  SetState = 7,
  // Reconcile the plugin chain to a new path list in place, reusing unchanged
  // instances, so a chain edit does not restart the whole host and drop audio.
  // Payload is ChainHeader followed by `count` null-terminated UTF-8 paths.
  SetChain = 8,
  // Enumerate one plugin's parameters (name/value/display/stable id) so the UI
  // can show a real rack. Request payload is ParamsHeader{pluginIndex}; the reply
  // is ParamsHeader{pluginIndex,paramCount,byteCount} + paramCount HostParamWire.
  GetParams = 9,
};

// Cap on parameters returned per query — a scrollable rack shows plenty within
// this, and it bounds the IPC message + the published region.
constexpr uint32_t kMaxParamsPerQuery = 256;

struct ParamsHeader {
  uint32_t pluginIndex = 0;
  uint32_t paramCount = 0;  // params in this reply (<= kMaxParamsPerQuery)
  uint32_t byteCount = 0;   // paramCount * sizeof(HostParamWire)
  uint32_t reserved = 0;
  char pluginName[48]{};    // the loaded plugin's display name (reply only)
};

// One parameter on the wire: fixed-size so the reply is a flat array. Strings are
// nul-padded and truncated; the engine hashes stableId to the durable uid it uses
// for param events, so a UI mapping survives a plugin version change.
struct HostParamWire {
  uint32_t index = 0;
  float normalized = 0.0f;   // current value, 0..1
  char stableId[48]{};       // plugin-stable id (JUCE param id), nul-padded
  char name[48]{};           // display name
  char display[24]{};        // current value text ("0.62", "440 Hz")
};

struct StateHeader {
  uint32_t pluginIndex = 0;
  uint32_t byteCount = 0;
};

struct ChainHeader {
  uint32_t count = 0;      // number of null-terminated paths that follow
  uint32_t byteCount = 0;  // total bytes of the path block
};

struct ControlHeader {
  uint32_t magic = kControlMagic;
  uint16_t version = kControlVersion;
  uint16_t type = 0;
  uint32_t size = 0;
  uint32_t reserved = 0;
};

struct HelloRequest {
  uint32_t blockSize = 0;
  uint32_t numChannelsIn = 0;
  uint32_t numChannelsOut = 0;
  uint32_t numBlocks = 0;
  uint32_t ringStdCapacity = 0;
  uint32_t ringCtrlCapacity = 0;
  uint32_t ringUiCapacity = 0;
  double sampleRate = 0.0;
};

struct HelloResponse {
  uint64_t shmSizeBytes = 0;
  char shmName[64]{};
};

struct ProcessBlockRequest {
  uint32_t blockId = 0;
  uint64_t engineSampleStart = 0;
  uint64_t pluginSampleStart = 0;
  uint16_t segmentStart = 0;
  uint16_t segmentLength = 0;
  uint32_t flags = 0;  // bit 0: transport is playing
  // Musical position for this block. A hosted plugin has no clock of its own,
  // so without these every tempo-synced effect free-runs.
  double bpm = 120.0;
  double ppqPosition = 0.0;
  double ppqPositionOfLastBarStart = 0.0;
  uint32_t timeSigNumerator = 4;
  uint32_t timeSigDenominator = 4;
};

constexpr uint32_t kProcessBlockFlagPlaying = 1u << 0;

struct OpenEditorRequest {
  uint32_t pluginIndex = 0;
  uint32_t reserved = 0;
};

struct SetBypassRequest {
  uint32_t pluginIndex = 0;
  uint32_t bypass = 0;
};

}  // namespace daw
