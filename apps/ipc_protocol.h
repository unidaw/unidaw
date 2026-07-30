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
// 5: SetParam — set one plugin parameter by durable uid16 (the UI rack's knob write).
//    A host without the handler would silently drop it, so gate it at the handshake.
// 6: GetBusLayout — the engine asks the host for a plugin's negotiated bus topology
//    (Movement 4). An old host would drop it, so it is handshake-gated like the rest.
// 7: GetLatency — the engine asks the host for the chain's processing latency (sum of
//    every plugin's getLatencySamples) so it can delay-compensate lower-latency tracks
//    against the highest-latency one (Movement 4 PDC). Host↔engine only; the frontend
//    contract (kShmVersion) is untouched. Handshake-gated so an old host can't answer
//    a query it doesn't implement with a misparsed reply.
// 8: ChainHeader gains sidechainMask — a per-plugin bit telling the host to enable that
//    plugin's sidechain (aux) input bus at prepare (Movement 4 sidechain). An old host
//    would misread the larger header, so gate it. Host↔engine only; kShmVersion stands.
// 9: HelloRequest gains numAuxChannelsOut (aux OUTPUT plane width) and ChainHeader gains
//    auxOutMask (enable a plugin's aux output buses) — Movement 4 multi-out. Both headers
//    grow, so an old host would misparse; gate it. Still host↔engine only; the aux plane
//    sits right after the main output plane at a computed offset, kShmVersion stands.
// 10: host->engine key ring (keystroke forwarding). The plugin-editor window fills a small
//    ring (EventType::HostKey) with keys the plugin didn't consume; the engine drains it
//    into transport/keyjazz. The ring sits right after the mailbox at a computed offset
//    (hostKeyRingOffset), so it needs no ShmHeader field — host↔engine only, kShmVersion
//    stands. Gated here because an old host wouldn't allocate/init the ring.
// v13: HostParamWire carries what a parameter IS — unit, default, range, the endpoint TEXTS,
// step count and flags. The wrapper already collected all of it; the wire dropped it.
constexpr uint16_t kControlVersion = 13;

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
  // Set one parameter on one plugin, keyed by the durable uid16 the UI got from
  // GetParams. Payload is SetParamRequest. Fire-and-forget (no reply); the host's
  // setter is an atomic store, safe from the control thread.
  SetParam = 10,
  // Enumerate one plugin's negotiated audio buses (Movement 4) so the engine can
  // stream bus topology to the UI. Request payload is BusLayoutHeader{pluginIndex};
  // the reply is BusLayoutHeader{pluginIndex,busCount,byteCount} + busCount
  // HostBusWire. Off the RT path, same shape as GetParams.
  GetBusLayout = 11,
  // Report the chain's processing latency (Movement 4 PDC). Request carries a
  // LatencyHeader whose contents the host ignores (the body must be non-empty — the
  // control loop only dispatches when header.size > 0). The reply is
  // LatencyHeader{pluginCount,totalSamples,byteCount} followed by pluginCount int32
  // per-plugin latencies. The engine aligns on totalSamples; per-plugin values are for
  // display. Off the RT path.
  GetLatency = 12,
  // PANIC support: reset every hosted plugin's internal DSP state (JUCE
  // AudioProcessor::reset) — the case a controller message cannot reach. CC120 tells a
  // plugin to stop sounding; a voice wedged inside the plugin's own state ignores it, and
  // that is precisely why panic exists. Fire-and-forget, no reply, no payload beyond the
  // header; the host does the work on its message thread, not the RT path.
  ResetPlugins = 13,
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
  // WHAT THE PARAMETER IS, not just where it is right now. All of this was already collected by
  // the wrapper (ParamInfo) and thrown away here, so a caller could read "Cutoff is 0.62,
  // displays 440 Hz" and had no way to know what 0.0 and 1.0 mean, whether it is a switch, or
  // what to reset it to. Setting a value in real units meant binary-searching `normalized` and
  // reading `display` back — which is the hallucination surface, not a workflow.
  char label[16]{};          // unit: "Hz", "dB", "%", "ms"
  float defaultNormalized = 0.0f;
  float minValue = 0.0f;     // the plugin's own range, when it exposes one
  float maxValue = 1.0f;
  // THE ENDPOINT TEXTS, and they are the ones that actually make a VST3 legible. A VST3 hosted
  // through JUCE usually reports a 0..1 normalisable range, so minValue/maxValue say nothing —
  // the real range only exists as TEXT. getText(0) and getText(1) give "20.0 Hz" and "20000 Hz",
  // which is what lets a caller (or an agent) reason in the units a musician uses.
  char minText[24]{};
  char maxText[24]{};
  uint32_t stepCount = 0;    // 0 = continuous; else the number of switch positions
  uint32_t flags = 0;        // kHostParamDiscrete | kHostParamAutomatable
};
constexpr uint32_t kHostParamDiscrete = 1u << 0;
constexpr uint32_t kHostParamAutomatable = 1u << 1;

// Cap on buses returned per GetBusLayout query (Movement 4). Matches
// kMaxBusesPerDevice in shared_memory.h; bounds the IPC reply.
constexpr uint32_t kMaxBusesPerQuery = 32;

struct BusLayoutHeader {
  uint32_t pluginIndex = 0;
  uint32_t busCount = 0;   // buses in this reply (<= kMaxBusesPerQuery)
  uint32_t byteCount = 0;  // busCount * sizeof(HostBusWire)
  uint32_t truncated = 0;  // 1 if the plugin had more buses than the cap
};

// One audio bus on the wire (host -> engine). Fixed-size so the reply is a flat array.
// channelOffset is the bus's first channel in the flat process buffer post-negotiation;
// layoutId is UiBusLayoutId; name is nul-padded/truncated.
struct HostBusWire {
  uint16_t flags = 0;        // bit0 isInput, bit1 isMain, bit2 enabled
  uint8_t index = 0;
  uint8_t channelCount = 0;
  uint16_t layoutId = 0;
  uint16_t channelOffset = 0;
  char name[24]{};
};

// GetLatency reply header (Movement 4 PDC). The host sums every plugin's reported
// processing latency into totalSamples — the value the engine delay-compensates on —
// and follows this header with `pluginCount` int32 per-plugin latencies for display.
// A plugin that reports no latency contributes 0; the sum is what a track's output is
// delayed by, so a lower-latency track is padded to match the chain's worst offender.
struct LatencyHeader {
  uint32_t pluginCount = 0;    // per-plugin values that follow (<= kMaxParamsPerQuery)
  uint32_t totalSamples = 0;   // sum of all plugins' latency = the track's chain latency
  uint32_t byteCount = 0;      // pluginCount * sizeof(int32_t)
  uint32_t reserved = 0;
};

// SetParam payload: set plugin[pluginIndex]'s parameter identified by uid16 to
// `normalized` (0..1). uid16 is the same durable key GetParams returns, so a mapping
// survives a plugin version change; the host resolves it to its stableId.
struct SetParamRequest {
  uint32_t pluginIndex = 0;
  uint8_t uid16[16]{};
  float normalized = 0.0f;
};

struct StateHeader {
  uint32_t pluginIndex = 0;
  uint32_t byteCount = 0;
};

struct ChainHeader {
  uint32_t count = 0;      // number of null-terminated paths that follow
  uint32_t byteCount = 0;  // total bytes of the path block
  // Movement 4 sidechain: bit i set = enable plugin[i]'s sidechain (aux) input bus at
  // prepare, so the engine can key a compressor off another track. The host feeds that
  // bus from the track's widened input plane. Bit set only for a plugin that both has
  // a sidechain route bound and declares an aux input bus; 0 = the pre-sidechain
  // behaviour (all non-main buses disabled).
  uint32_t sidechainMask = 0;
  // Movement 4 multi-out: bit i set = enable plugin[i]'s aux OUTPUT buses at prepare, so
  // a multi-out instrument's stems reach the aux output plane. Bit set only when the
  // engine wants that plugin's buses split to child tracks; 0 = aux outputs disabled.
  uint32_t auxOutMask = 0;
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
  // Movement 4 multi-out: channels reserved for the AUX OUTPUT plane, which the host
  // lays out immediately after the main output plane. A multi-out instrument's aux
  // buses (a drum plugin's stems) are written there; the engine reads each bus's slice
  // for its child track. 0 = no aux plane (the pre-multi-out layout). numChannelsOut
  // stays the MAIN width, so the master mix + sidechain offset are unchanged.
  uint32_t numAuxChannelsOut = 0;
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
