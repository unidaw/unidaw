#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <array>
#include <map>
#include <memory>
#include <algorithm>
#include <tuple>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <limits>
#include <unordered_map>

#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "platform_juce/juce_wrapper.h"
#include "apps/audio_shm.h"
#include "apps/engine_instance.h"
#include "apps/engine_types.h"
#include "apps/engine_producer_helpers.h"
#include "apps/engine_audio_callback.h"
#include "apps/engine_audio_start.h"
#include "apps/engine_offline_render.h"
#include "apps/engine_shutdown.h"
#include "apps/engine_song_store.h"
#include "apps/engine_ui_shm.h"
#include "apps/engine_produce_block.h"
#include "apps/engine_startup.h"
#include "apps/engine_producer_thread.h"
#include "apps/engine_bulk_edit.h"
#include "apps/engine_consumer.h"
#include "apps/engine_publish_clips.h"
#include "apps/engine_clip_edit.h"
#include "apps/engine_track_setup.h"
#include "apps/engine_arrange_markers.h"
#include "apps/engine_track_table.h"
#include "apps/engine_patcher_graph_owner.h"
#include "apps/engine_song_extent.h"
#include "apps/engine_song_timing.h"
#include "apps/engine_transport_state.h"
#include "apps/engine_chain_host.h"
#include "apps/engine_track_rebuild.h"
#include "apps/engine_restart_worker.h"
#include "apps/engine_master_render.h"
#include "apps/engine_ui_publish.h"
#include "apps/engine_patcher_assemble.h"
#include "apps/engine_ui_thread.h"
#include "apps/engine_xrun_reporter.h"
#include "apps/engine_handle_ui_entry.h"
#include "apps/engine_harmony_timeline.h"
#include "apps/engine_load_project.h"
#include "apps/engine_render_track.h"
#include "apps/engine_save_project.h"
#include "apps/worker_pool.h"
#include "apps/engine_pure.h"
#include "apps/engine_rt_helpers.h"
#include "apps/engine_automation_commands.h"
#include "apps/engine_clip_commands.h"
#include "apps/engine_modlink_commands.h"
#include "apps/engine_module_commands.h"
#include "apps/engine_patcher_commands.h"
#include "apps/engine_chain_commands.h"
#include "apps/engine_marker_commands.h"
#include "apps/engine_project_commands.h"
#include "apps/engine_rowops_commands.h"
#include "apps/engine_track_commands.h"
#include "apps/engine_request_commands.h"
#include "apps/engine_trackprops_commands.h"
#include "apps/engine_device_commands.h"
#include "apps/engine_note_commands.h"
#include "apps/engine_undo_commands.h"
#include "apps/engine_sampler_commands.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/rt_thread.h"
#include "apps/render_pool.h"
#include "apps/host_controller.h"
#include "apps/plugin_cache.h"
#include "apps/patcher_abi.h"
#include "apps/audio_region.h"
#include "apps/clip_grid.h"
#include "apps/waveform_store.h"
#include "apps/patcher_assemble.h"
#include "apps/patcher_graph.h"
#include "apps/patcher_preset.h"
#include "apps/patcher_preset_library.h"
#include "apps/event_log.h"
#include "apps/project_file.h"
#include "apps/device_chain.h"
#include "apps/modulation.h"
#include "apps/track_routing.h"
#include "apps/watchdog.h"
#include "apps/latency_manager.h"
#include "apps/time_base.h"
#include "apps/lane_quantize.h"
#include "apps/markers.h"
#include "apps/ripple.h"
#include "apps/sampler_engine.h"
#include "apps/sampler_slice.h"
#include "apps/musical_structures.h"
#include "apps/placement_schedule.h"
#include "apps/note_entry.h"
#include "apps/placement_flatten.h"
#include "apps/automation_clip.h"
#include "apps/uid_hash.h"
#include "apps/scale_library.h"
#include "apps/harmony_timeline.h"
#include "apps/chord_resolver.h"
#include "apps/ui_snapshot.h"
#include "apps/clip_edit.h"

// The pure helpers that used to be lambdas in main(). They are unqualified at ~31 call sites and
// stay that way: this keeps the extraction a pure move, and any name that failed to resolve — or
// resolved to something else — is a compile error rather than a silent behaviour change.
using namespace daw::engine;

namespace {

// Keystroke forwarding: map a forwarded editor key (JUCE key code, uppercase-ASCII for
// letters) to a MIDI pitch using the classic tracker keyboard — the Z row is the lower
// octave (base C4 = 60), the Q row the octave above. Returns -1 for a non-note key.

// The per-instance path derivations live in apps/engine_instance.h — engineInstanceToken,
// trackSocketPath, trackShmName and uiShmName. They are in a header because a ctest harness
// hardcoded "/daw_engine_shared" and broke the moment the engine started deriving the name;
// duplicating the derivation there would have been the same bug from the other side.
using daw::engineInstanceToken;
using daw::trackShmName;
using daw::trackSocketPath;
using daw::uiShmName;


// A machine-level cache location, found regardless of the current directory, so a
// checkout that has scanned once is not silently cacheless when run from elsewhere.
// Honors XDG_CACHE_HOME (portable), else the macOS app-support dir, else ~/.cache.
std::string stablePluginCachePath() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    return std::string(xdg) + "/uni/plugin_cache.json";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
#if defined(__APPLE__)
    return std::string(home) + "/Library/Application Support/uni/plugin_cache.json";
#else
    return std::string(home) + "/.cache/uni/plugin_cache.json";
#endif
  }
  return {};
}

std::string defaultPluginCachePath() {
  if (const char* env = std::getenv("DAW_PLUGIN_CACHE")) {
    return env;
  }
  if (std::filesystem::exists("build/plugin_cache.json")) {
    return "build/plugin_cache.json";
  }
  if (std::filesystem::exists("../build/plugin_cache.json")) {
    return "../build/plugin_cache.json";
  }
  // Machine-level fallback before the bare cwd name: a fresh checkout run from any
  // directory still finds a cache it scanned earlier. Kept AFTER the cwd build paths
  // so the local dev build->run loop is unchanged; making it authoritative over cwd
  // is a separate, coordinated change (the launcher owns the write side).
  if (const auto stable = stablePluginCachePath();
      !stable.empty() && std::filesystem::exists(stable)) {
    return stable;
  }
  return "plugin_cache.json";
}




// kEventFlagMusicalLogic and priorityForEvent moved to apps/engine_rt_helpers.h — the flag is
// part of the wire contract and belonged in a header, and the ordering rule now has tests.

// Audio callback for mixing and outputting audio from all tracks


}  // namespace

int main(int argc, char** argv) {
  // Never let a dead host take the engine down. macOS doesn't define
  // MSG_NOSIGNAL, so send() to a host socket that just closed raises SIGPIPE,
  // whose default action is to terminate the process — which is exactly what
  // happened when a plugin host died mid-playback. Ignoring it turns those writes
  // into EPIPE returns, which the IPC layer already handles by marking the host
  // dead and scheduling a restart.
  std::signal(SIGPIPE, SIG_IGN);

  // WHAT argv DECIDES lives in one struct and is parsed by one function; see
  // apps/engine_startup.h for the two flag rules that are there because they were wrong.
  daw::engine::EngineArgs engineArgs;
  engineArgs.socketPath = trackSocketPath(0);
  if (const int rc = daw::engine::parseEngineArgs(argc, argv, engineArgs); rc != 0) {
    return rc;
  }
  auto& pluginPath = engineArgs.pluginPath;
  auto& spawnHost = engineArgs.spawnHost;
  auto& runSeconds = engineArgs.runSeconds;
  auto& renderName = engineArgs.renderName;
  auto& forcedBlockSize = engineArgs.forcedBlockSize;
  auto& forcedSampleRate = engineArgs.forcedSampleRate;
  auto& startupProject = engineArgs.startupProject;
  auto& testMode = engineArgs.testMode;

  if (const int rc = daw::engine::checkHostBinaryVersion(); rc != 0) {
    return rc;
  }

  // OFFLINE RENDER state, declared here so the producer thread below can capture it.
  const bool offlineRender = !renderName.empty();
  int offlineChannels = 2;  // the master width the pump renders at; set when the mix is wired
  bool renderFailed = false;  // a stalled render must exit non-zero, not just warn
  // DETERMINISM GATE. The producer starts as soon as a host is ready and runs free while
  // audioPlaybackBlockId is still 0 (there is nothing to pace to yet), filling the ring with
  // blocks produced BEFORE the transport was started. How many depends on how fast the hosts
  // came up, so the render's first blocks varied run to run and two renders of one project were
  // not byte-identical — which the determinism assertion in offline_render_check caught on its
  // first run. Offline holds the producer until the pump has started the transport, so block 1
  // is always tick 0 and block N is always N blocks in.
  std::atomic<bool> offlineProducerArmed{false};

  // The environment says the rest. Same struct, same function: a knob is a startup option
  // whether it arrived as a flag or as an export, and splitting them across two places is how
  // `testMode` ended up settable from one and documented in neither.
  daw::engine::readStartupEnvironment(engineArgs);
  auto& testThrottleMs = engineArgs.testThrottleMs;
  auto& patcherParallel = engineArgs.patcherParallel;
  auto& pdcDisabled = engineArgs.pdcDisabled;
  auto& traceNotes = engineArgs.traceNotes;
  std::unique_ptr<daw::engine::WorkerPool> patcherPool;
  if (patcherParallel) {
    size_t threadCount = std::max<size_t>(1, std::thread::hardware_concurrency());
    if (const char* env = std::getenv("DAW_PATCHER_PARALLEL_THREADS")) {
      char* end = nullptr;
      const long value = std::strtol(env, &end, 10);
      if (end != env && value > 0) {
        threadCount = static_cast<size_t>(value);
      }
    }
    patcherPool = std::make_unique<daw::engine::WorkerPool>(threadCount);
  }

  // The device and the flags together decide the config; see apps/engine_startup.h for why
  // the flag overrides are applied AFTER the probe rather than before it.
  daw::engine::EngineDevice engineDevice = daw::engine::openAudioDevice(engineArgs);
  auto& baseConfig = engineDevice.baseConfig;
  auto& audioRuntime = engineDevice.audioRuntime;
  auto& audioBackend = engineDevice.audioBackend;
  const uint32_t uiDiffRingCapacity = 1024;

  const std::string pluginCachePath = defaultPluginCachePath();
  const auto pluginCache = daw::readPluginCache(pluginCachePath);
  std::cout << "Plugin cache: " << pluginCachePath
            << " (" << pluginCache.entries.size() << " entries)" << std::endl;

  auto resolvePluginIndex = [&](const std::string& path) -> std::optional<uint32_t> {
    if (path.empty()) {
      return std::nullopt;
    }
    std::error_code ec;
    const auto target = std::filesystem::weakly_canonical(path, ec);
    for (size_t i = 0; i < pluginCache.entries.size(); ++i) {
      const auto& entry = pluginCache.entries[i];
      if (entry.path.empty()) {
        continue;
      }
      const auto entryPath = std::filesystem::weakly_canonical(entry.path, ec);
      if (entryPath == target || entry.path == path) {
        return static_cast<uint32_t>(i);
      }
    }
    return std::nullopt;
  };


  // The TYPE moved to apps/engine_types.h; the VARIABLE stays here, because it always was a
  // main() local. It was written `struct UiShmState { ... } uiShm;` — one statement declaring a
  // type and defining an object — so hoisting it wholesale put a global in every translation
  // unit that included the header, and the second one to do so failed to link. Splitting the two
  // is what that shape actually needs.
  UiShmState uiShm;

  if (const int rc = daw::engine::setUpUiShm(uiShm, baseConfig, uiDiffRingCapacity);
      rc != 0) {
    return rc;
  }




  auto buildTrackSnapshot = [&](const Track& track)
      -> std::shared_ptr<const TrackStateSnapshot> {
  auto snapshot = std::make_shared<TrackStateSnapshot>();
  snapshot->chainDevices = track.chain.devices;
  snapshot->modLinks = track.modRegistry.links;
  snapshot->routing = track.routing;
  snapshot->automationClips = track.automationClips;
  snapshot->harmonyQuantize = track.harmonyQuantize;
  snapshot->soundAddressedOnly = track.soundAddressedOnly;
  return snapshot;
};




  daw::engine::TrackSetupDeps trackSetupDeps{
      baseConfig, buildTrackSnapshot, resolvePluginIndex};

  auto setupTrackRuntime = [&](uint32_t trackId, const std::string& trackPluginPath,
                               bool allowConnect, bool startHost)
      -> std::unique_ptr<TrackRuntime> {
    return daw::engine::setupTrackRuntime(trackSetupDeps, trackId, trackPluginPath,
                                          allowConnect, startHost);
  };

  daw::engine::TrackTable trackTable;
  auto& tracks = trackTable.tracks;
  tracks.reserve(daw::kUiMaxTracks);
  auto& tracksMutex = trackTable.tracksMutex;

  // Movement 4: how many tracks the UI should see. The `tracks` vector only ever grows
  // (a runtime is reused, never removed), so publishing tracks.size() leaves phantom
  // lanes from a larger project loaded before a smaller one. This is set to the loaded
  // document's track count and extended as aux children are appended, so the published
  // count is honest. Starts equal to whatever the startup creates.
  std::atomic<uint32_t> liveTrackCount{0};
  // True while a project load is mutating the track set (adopting document tracks,
  // tearing down leftovers, setting liveTrackCount). The consumer defers deriving aux
  // children until it clears, so a child is never placed against a half-updated track
  // set — e.g. before the load-clear has torn down the leftover it would recycle.
  std::atomic<bool> loadInProgress{false};
  // ONE definition of "the save will write this track", shared by the save itself and by
  // the commands that author persistent data on a track. Three separate kinds of runtime
  // are skipped at save time — an aux child (derived from the plugin's bus layout, never
  // persisted), a tombstone (a hole kept only to hold an id), and a slot past the live
  // count (a leftover of a larger project) — and a handler that checks only `trackId <
  // tracks.size()` accepts an edit to all three. The edit is then applied, reported as
  // applied, and silently absent after the next reload, with nothing anywhere saying so.
  // Keeping the predicate in one place is what stops the two from drifting apart again.
  auto trackIsPersisted = [&](const TrackRuntime& rt) {
    return !rt.isAuxChild.load(std::memory_order_acquire) &&
           !rt.removed.load(std::memory_order_acquire) &&
           rt.trackId < liveTrackCount.load(std::memory_order_acquire);
  };
  // Everything a track CONTAINS, wiped in one place. The caller must already hold
  // runtime->trackMutex.
  //
  // Three paths repurpose an existing runtime — AddTrack refilling a tombstone, the load
  // blanking a slot past the new document, and reconcileChildTracks recycling a slot as a
  // stem — and all three cleared the same four fields by hand (chain, placements, owned
  // clips, editable ids) while all three forgot the same two: `automationClips` and
  // `modRegistry.links`. Neither is cleared anywhere else either; both are only ever
  // ASSIGNED, at load, for tracks the document actually names.
  //
  // So: remove a track that had a filter sweep and a mod link, add a track, and the new
  // track carries the deleted one's automation and a link naming device ids that no longer
  // exist — device ids restart per track, so the leftover link can end up modulating
  // whatever device now sits in that slot. Both are then written to disk by the next save.
  // Three copies of a list that has to stay complete is the bug; one function is the fix.
  auto resetTrackContent = [](TrackRuntime& rt) {
    rt.track.chain = daw::TrackChain{};
    rt.track.modRegistry.links.clear();
    rt.track.automationClips.clear();
    rt.track.harmonyQuantize = false;
    rt.track.soundAddressedOnly = false;
    rt.sourcePlacements.clear();
    rt.ownedClips.clear();
    rt.editableClipIds.clear();
    rt.arrangementDirty.store(false, std::memory_order_relaxed);
    // Lane settings and the mixer belong to the track that is gone, not to whatever takes
    // the slot next. A leftover solo is the worst of these: the whole project goes quiet
    // and the reason is on a lane the user thinks they deleted.
    rt.mixGainLinear.store(1.0f, std::memory_order_relaxed);
    rt.mixPan.store(0.0f, std::memory_order_relaxed);
    rt.mixMute.store(false, std::memory_order_relaxed);
    rt.mixSolo.store(false, std::memory_order_relaxed);
    rt.quantizeGrid.store(0, std::memory_order_release);
    rt.quantizeStrength.store(0, std::memory_order_release);
    rt.quantizeSwing.store(0, std::memory_order_release);
    rt.linesPerBeat.store(4, std::memory_order_relaxed);
    rt.allowNoteOverlap.store(false, std::memory_order_relaxed);
  };

  std::map<std::pair<uint32_t, uint32_t>, AuxChildOverlay> auxChildOverlays;
  std::mutex auxChildOverlayMutex;
  TrackRuntime* uiTrack = nullptr;
  {
    auto runtime = setupTrackRuntime(0, pluginPath, !spawnHost, true);
    if (!runtime) {
      daw::LogLine() << "Failed to connect to host." << std::endl;
      return 1;
    }
    uiTrack = runtime.get();
    tracks.push_back(std::move(runtime));
  }
  daw::LogLine() << "Engine: track runtime(s) ready, starting threads" << std::endl;
  if (testMode) {
    constexpr uint32_t kTestTrackCount = 3;
    for (uint32_t trackId = 1; trackId < kTestTrackCount; ++trackId) {
      auto runtime = setupTrackRuntime(trackId, pluginPath, true, false);
      if (!runtime) {
        daw::LogLine() << "Failed to launch test track " << trackId << "." << std::endl;
        return 1;
      }
      tracks.push_back(std::move(runtime));
    }
  }

  liveTrackCount.store(static_cast<uint32_t>(tracks.size()),
                       std::memory_order_relaxed);

  // patcher-is-a-device item 4: the MASTER track. A real device chain + mixer whose
  // output is the master bus, addressable by kMasterTrackId. Kept OUT of the `tracks`
  // vector so it never collides with AddTrack/RemoveTrack/aux-child slot logic;
  // published compacted after the regular tracks and addressed by its stable id. A
  // separate runtime with no clips; VST effects on the master SUM (its host) arrive
  // in 4b, so for now it holds patcher/mod devices and is a visible, selectable home
  // for a global patcher.
  auto masterTrack = std::make_unique<TrackRuntime>();
  masterTrack->trackId = daw::kMasterTrackId;
  masterTrack->trackName = "Master";
  masterTrack->trackSnapshot = buildTrackSnapshot(masterTrack->track);
  // 4b groundwork: give the master a host-capable config so a VST effect on the master
  // SUM can be hosted out of process. Its input IS the sum, so numChannelsIn ==
  // numChannelsOut (an audio-in effects chain). Dedicated socket/shm names off the
  // master id. No host is launched until it actually has a VST effect (reconcileMasterHost).
  masterTrack->config = baseConfig;
  masterTrack->config.socketPath = trackSocketPath(daw::kMasterTrackId);
  masterTrack->config.shmName = trackShmName(daw::kMasterTrackId);
  masterTrack->config.numChannelsIn = masterTrack->config.numChannelsOut;
  masterTrack->config.pluginPaths.clear();
  masterTrack->config.pluginNames.clear();
  // 4b gate (first half): the master has an enabled VST effect. The callback ANDs this
  // with the master host being ready. Set by reconcileMasterHost; read by the callback
  // via a wired pointer.
  std::atomic<bool> masterFxActive{false};

  daw::LatencyManager latencyMgr;
  const auto& engineConfig = tracks.front()->config;
  latencyMgr.init(engineConfig.blockSize, engineConfig.numBlocks);
  std::cout << "System latency: " << latencyMgr.getLatencySamples()
            << " samples (" << (engineConfig.numBlocks > 0 ? engineConfig.numBlocks - 1 : 0)
            << " blocks)" << std::endl;

  // Track audio playback position for synchronization
  std::atomic<uint32_t> audioPlaybackBlockId{0};
  // Last steady-state pipeline depth (producer blocks ahead of the device) sampled by the
  // reporter while playing — the transport-to-ear latency in blocks.
  std::atomic<uint32_t> observedPipelineBlocks{0};

  // PRODUCER LOAD. The producer builds each block one block ahead of the device, so the whole
  // pipeline holds together only while producing a block costs LESS than a block lasts. Past
  // 1.0x it cannot catch up by definition: every block it falls further behind, the ring
  // drains, and the callback starts dropping tracks.
  //
  // The owner's standing directive on this is "many sampler tracks saturating one producer
  // thread MUST NEVER HAPPEN", and a directive you cannot measure is a hope. This is the
  // measurement: wall-clock microseconds per produced block, the sampler DSP's share of it,
  // the worst single block, and how many blocks went over budget. Load is
  // producerBlockUsTotal / blocks / blockDurationUs.
  //
  // Counted, not sampled — a sampler that blows the budget on the one block where 64 voices
  // start together is exactly the case a periodic sample misses. Written only by the producer
  // thread, read by the reporter and the shutdown summary, so relaxed is enough.
  std::atomic<uint64_t> producerBlocksTimed{0};
  std::atomic<uint64_t> producerBlockUsTotal{0};
  std::atomic<uint64_t> producerBlockUsMax{0};
  std::atomic<uint64_t> producerSamplerUsTotal{0};
  std::atomic<uint64_t> producerSamplerUsMax{0};
  std::atomic<uint64_t> producerBlocksOverBudget{0};

  // The pool the per-track work runs on. Sized to leave the audio callback, the master render
  // thread and the OS room to breathe rather than claiming every core — a producer that
  // finishes a block fractionally sooner by starving the thread that PLAYS it has made things
  // worse. DAW_ENGINE_RENDER_THREADS overrides; 0 or 1 keeps everything on the producer thread,
  // which is also the reference the parallel path is checked for bit-identical output against.
  daw::RenderPool renderPool;
  // WHETHER TO USE IT THIS BLOCK, and it is not "always". Measured on a real device: at 8 sampler
  // tracks one thread spends 0.18x of the block budget and has room to spare, and waking seven
  // workers every block to help costs MORE than it saves — across four runs the pool dropped
  // 4/0/2/7 callbacks where one thread dropped 0/3/0/0. Those workers compete for cores with the
  // audio callback itself, which is the one thread that must never wait.
  //
  // So the pool engages on the WORK, not on the track count. The signal is summed sampler CPU per
  // block, which is the serial-equivalent cost and therefore means the same thing whichever mode
  // is currently running — a wall-clock signal would read low BECAUSE the pool was on and
  // oscillate the moment it turned off.
  bool poolAlwaysOn = false;
  bool poolEngaged = false;
  double poolWorkEwmaUs = 0.0;
  {
    const unsigned hw = std::thread::hardware_concurrency();
    unsigned want = hw > 3 ? hw - 2 : 1;
    if (const char* env = std::getenv("DAW_ENGINE_RENDER_THREADS")) {
      const int n = std::atoi(env);
      want = n > 0 ? static_cast<unsigned>(n) : 1;
    }
    if (want > 1) {
      renderPool.start(want - 1);  // the producer thread is the other worker
    }
    // AN EXPLICIT COUNT MEANS "I KNOW WHAT I WANT" and turns the adaptive rule off, which is
    // also how a test forces the pool on regardless of how little work its fixture makes.
    poolAlwaysOn = std::getenv("DAW_ENGINE_RENDER_THREADS") != nullptr && want > 1;
    std::cout << "Render pool: " << (renderPool.workerCount() + 1)
              << " thread(s) for per-track production"
              << (poolAlwaysOn ? " (forced)" : " (engaged when the work needs it)") << std::endl;
  }

  std::unique_ptr<EngineAudioCallback> audioCallback;
  // PUBLISHED SEPARATELY, because the producer and consumer threads are created LONG before this
  // is assigned — the callback needs the device's sample rate and block size, and the device is
  // opened later. Both threads tested `if (audioCallback)` while main was writing it, which
  // ThreadSanitizer reported as a data race and which is not the harmless kind: a reader can see
  // the pointer before the constructor's stores are visible and then dereference it.
  //
  // The unique_ptr keeps OWNERSHIP on the main thread and never leaves it. This is the
  // PUBLICATION: stored with release once the callback is fully constructed AND configured, read
  // with acquire by the threads, so seeing a non-null pointer means seeing a finished object.
  // Null until then, which every reader already handles — that was never the bug.
  std::atomic<EngineAudioCallback*> audioCallbackPublished{nullptr};
  auto publishedCallback = [&]() -> EngineAudioCallback* {
    return audioCallbackPublished.load(std::memory_order_acquire);
  };
  // 4b: drives the master host one block behind the callback. Started once the callback
  // exists (below), joined at shutdown.
  std::thread masterRenderThread;

  // Map-aware so a loaded project's tempo — including changes mid-song — actually
  // takes effect. A StaticTempoProvider here made the engine play every project at
  // 120 regardless of its tempo_map.
  daw::TempoMapProvider tempoProvider(120.0);
  daw::NanotickConverter tickConverter(
      tempoProvider, static_cast<uint32_t>(engineConfig.sampleRate));
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;
  const uint64_t patternRows = 16;  // Loop first bar until loop range is configurable
  const uint64_t rowNanoticks = ticksPerBeat / 4;
  const uint64_t patternTicks = rowNanoticks * patternRows;

  const uint32_t maxUiTracks = daw::kUiMaxTracks;
  // No test notes - wait for user input from the tracker
  std::cout << "Engine: Ready for tracker input" << std::endl;

  daw::engine::PatcherGraphOwner patcherGraph;
  // Has anyone actually EDITED the shared pool this session?
  //
  // The save's legacy branch parks the pool on the first instrument so the one global graph
  // the engine used to run round-trips. But the engine seeds that pool at startup with a
  // demo graph (Euclidean 16/5 + Passthrough + AudioPassthrough), so the branch fired for any
  // project that had a device and no per-device graph of its own — and loading a plain
  // one-instrument project and saving it stamped three patcher nodes onto the user's
  // instrument that they never created. Verified: a fixture with zero patcher data anywhere
  // came back with ['euclidean', 'passthrough', 'audio_passthrough']. Not audible in that
  // configuration, but it is authored-looking data invented by a save, and it flips
  // documentHasPerDeviceGraphs on the next load so the second save takes a different branch
  // than the first.
  //
  // Parking a pool the user edited is the round-trip this branch exists for; parking the boot
  // default is just litter. Once the patcher's edit commands are per-device (they still
  // address the pool — the largest remaining gap in "patcher is a device") this never becomes
  // true and the branch can go.
  // True when the running pool was assembled from per-device graphs (>= 2 devices
  // each carrying one) at load. Save then preserves each device's own graph rather
  // than parking the live single graph on one device (the legacy path).
  {
    std::lock_guard<std::mutex> lock(patcherGraph.patcherGraphState.mutex);
    daw::PatcherNode euclid;
    euclid.id = 0;
    euclid.type = daw::PatcherNodeType::Euclidean;
    euclid.hasEuclideanConfig = true;
    euclid.euclideanConfig.steps = 16;
    euclid.euclideanConfig.hits = 5;
    euclid.euclideanConfig.offset = 0;
    euclid.euclideanConfig.duration_ticks = 0;
    euclid.euclideanConfig.degree = 1;
    euclid.euclideanConfig.octave_offset = 0;
    euclid.euclideanConfig.velocity = 100;
    euclid.euclideanConfig.base_octave = 4;
    patcherGraph.patcherGraphState.graph.nodes.push_back(euclid);

    daw::PatcherNode passthrough;
    passthrough.id = 1;
    passthrough.type = daw::PatcherNodeType::Passthrough;
    patcherGraph.patcherGraphState.graph.nodes.push_back(passthrough);

    daw::PatcherNode audioNode;
    audioNode.id = 2;
    audioNode.type = daw::PatcherNodeType::AudioPassthrough;
    patcherGraph.patcherGraphState.graph.nodes.push_back(audioNode);

    daw::PatcherEdge edge{};
    edge.src = {0, daw::kPatcherEventOutputPort};
    edge.dst = {1, daw::kPatcherEventInputPort};
    edge.kind = daw::PatcherPortKind::Event;
    patcherGraph.patcherGraphState.graph.edges.push_back(edge);
  }
  if (!daw::buildPatcherGraph(patcherGraph.patcherGraphState.graph)) {
    daw::LogLine() << "Patcher graph invalid; disabling patcher kernels." << std::endl;
    std::lock_guard<std::mutex> lock(patcherGraph.patcherGraphState.mutex);
    patcherGraph.patcherGraphState.graph.nodes.clear();
    patcherGraph.patcherGraphState.graph.edges.clear();
    patcherGraph.patcherGraphState.graph.topoOrder.clear();
    patcherGraph.patcherGraphState.graph.depths.clear();
    patcherGraph.patcherGraphState.graph.resolvedInputs.clear();
    patcherGraph.patcherGraphState.graph.idToIndex.clear();
    patcherGraph.patcherGraphState.graph.maxDepth = 0;
    patcherGraph.patcherGraphState.nextNodeId = 0;
  }
  // Declared here rather than beside the reassembly forwarder below, because a deps struct at
  // line ~2830 takes it as a std::function and the track list it would otherwise need does not
  // exist yet. Publishing the snapshot needs only the graph, so it can be available this early.
  auto updatePatcherGraphSnapshot = [&] {
    daw::engine::updatePatcherGraphSnapshot(patcherGraph);
  };
  updatePatcherGraphSnapshot();

  daw::engine::TransportState transport;
  // TICKS PLAYED SINCE THE TRANSPORT LAST STARTED FROM THE LOOP START, never wrapped.
  //
  // transportNanotick is a POSITION and wraps at the loop end, so nothing in the engine knew
  // which PASS it was on — and conditional trigs (`1:2`, `3:4`) are defined entirely in terms of
  // that. This is the one place the pass index comes from.
  //
  // IT MUST NOT BE A COUNTER THE DISPATCH INCREMENTS. Advanced only here, by the same blockTicks
  // the transport advances by, so it is a function of the transport rather than of how many
  // times a code path happened to run. A counter bumped per dispatch would depend on when
  // playback started and how the blocks fell, and an offline bounce would quietly stop being
  // reproducible while passing every structural test in the suite.
  //
  // Reset with the position on any explicit seek, so pass counting restarts from wherever you
  // dropped the playhead — and so a render, which always begins at the loop start, always begins
  // at pass 0.
  std::atomic<bool> resetTimeline{false};
  std::mutex restartMutex;
  std::condition_variable restartCv;
  std::deque<TrackRuntime*> restartQueue;
  transport.loopEndNanotick.store(patternTicks, std::memory_order_release);
  std::atomic<bool> clipDirty{true};
  // The lane's quantize, read from the one place it lives. Used by BOTH the scheduling
  // copy and the published deviation, so the number the UI draws and the number the
  // audio uses cannot come from different settings.
  auto laneQuantizeOf = [](const TrackRuntime& rt) -> daw::LaneQuantize {
    daw::LaneQuantize q;
    q.gridNanoticks = rt.quantizeGrid.load(std::memory_order_acquire);
    q.strengthMilli = rt.quantizeStrength.load(std::memory_order_acquire);
    q.swingMilli = rt.quantizeSwing.load(std::memory_order_acquire);
    return q;
  };

  std::atomic<uint32_t> clipVersion{0};
  // M1.13: moves when a LANE's quantize changes. Deliberately separate from
  // clipVersion — quantize moves no authored note, so it must not invalidate anyone's
  // in-flight edit, but the UI still has to redraw its deviation bars.
  std::atomic<uint32_t> quantizeVersion{0};
  // M3: the SONG's end — the furthest placement end across every track — kept apart
  // from the LOOP. Before this they were the same number, set only at load, so adding a
  // placement past the end left the loop where it was and the new material NEVER PLAYED:
  // you would add a section at bar 4, press play, and hear nothing, with no explanation
  // anywhere. Recomputed whenever a placement edit changes the arrangement.
  daw::engine::SongTiming songTiming;
  // v29: THE ARRANGEMENT — named positions and the song's meter. Its own version counter,
  // deliberately NOT clipVersion: renaming a marker moves no note, so it must not invalidate
  // anyone's in-flight edit — the same separation quantizeVersion has.
  //
  // ONE MUTEX FOR BOTH, and that is a simplification the spine could not have. The old pair
  // (sectionMutex + songMeterMutex) had to be held NESTED because deriving a section's position
  // needed both — the spine said how many bars, the meter said how long a bar is — and the first
  // version took them in one order in the arrangement publisher and the other in
  // SetSectionLength. That is an AB/BA deadlock a few instructions wide: it never fired in a test
  // and would have wedged the engine mid-edit with no diagnostic. Moving the meter onto the
  // section deleted one of the two; deleting the section deletes the derivation itself, so a
  // marker's bar is a lookup in the map and there is no pair left to invert.
  daw::engine::ArrangeMarkers arrange;
  // AN RT-SAFE COPY OF THE METER, for the audio/host thread. The play head has to report the
  // signature at the PLAYHEAD, not the song default — that is the whole point of an authoritative
  // meter map, and reporting the default is the bug this replaces. The RT cannot take arrangeMutex,
  // so the map is published as an immutable snapshot and swapped atomically, exactly like
  // trackSnapshot. Never null after startup.
  // WHERE A BAR STARTS AND ENDS, according to the song's meter. The rule and the reasons are in
  // apps/engine_pure.h (`barEndTick`), where they can be tested; these two lambdas exist only to
  // supply the meter.
  //
  // WHICH METER: the SONG's. #76 put the meter on the song and kept the grid on the clip, and
  // #79 flattened it to markers — so "song or clip meter" is not open, it was answered by those
  // two rulings.
  //
  // READ FROM THE SNAPSHOT, NOT songMeter, and that is not merely convenient. songMeter is under
  // arrangeMutex and these callers hold trackMutex; taking the pair nested is the AB/BA deadlock
  // the comment above says was deleted when the section went away. The snapshot is swapped
  // atomically and needs no lock, which is why it exists — and passing its raw pointer in is what
  // lets the rule itself be a pure function.
  auto barEndTick = [&](uint64_t tick) -> uint64_t {
    return daw::engine::barEndTick(
        std::atomic_load_explicit(&songTiming.meterSnapshot, std::memory_order_acquire).get(), tick);
  };
  auto barStartTick = [&](uint64_t tick) -> uint64_t {
    return daw::engine::barStartTick(
        std::atomic_load_explicit(&songTiming.meterSnapshot, std::memory_order_acquire).get(), tick);
  };
  // The song's bar grid, for note entry and for segmenting a flat track into clips. Both used to
  // take a bar LENGTH and compute (tick / length) * length, which is right in one meter and wrong
  // in every project with a signature change: the bar containing a tick is then at a multiple of
  // nothing, so new clips anchored that way land off the ruler the user is reading.
  auto songBarGrid = [&]() -> daw::BarGrid {
    return daw::BarGrid{[&](uint64_t tick) { return barStartTick(tick); }};
  };
  // v28: moves whenever ANY automation changes — a point written, a lane created, a ripple that
  // moved points, a load, a slot reused. Deliberately NOT the clip version: automation is not
  // notes, and a client caching lanes on the clip version would re-read them on every keystroke.
  // Same separation sectionVersion and quantizeVersion already have.
  std::atomic<uint32_t> automationVersion{0};
  // songTimeSigNum/Den below are the map's FIRST point, kept as their own fields because the
  // header, the TransportPayload and the play head all read them and because every file written
  // before the map existed means exactly this. A project in one meter has an empty map and these
  // two numbers; a project with a 7/8 bridge has both, and the MAP wins.

  // Whether the loop was set BY HAND. The loop follows the song end only while it was
  // not — otherwise every note you type would silently reset a loop you had chosen,
  // which is the opposite failure and a worse one.

  // M2.17: bump BOTH counters for a track-scoped change — the track's (what acceptance
  // compares, and what the diff hands back to the caller as its new base) and the global
  // (the "something moved" signal every publisher polls to know its region is stale).
  // One helper so a bump site cannot advance one and forget the other: forgetting the
  // track counter makes that track's edits succeed forever regardless of base, and
  // forgetting the global freezes the published regions so the edit is never visible.
  // Returns the track's NEW version.
  //
  // Two entry points because of lock order. Code already holding a track's trackMutex
  // must not reach for tracksMutex (every other path takes tracksMutex first, briefly,
  // then trackMutex — taking them the other way round is the classic inversion), so
  // those sites pass the TrackRuntime* they already hold. TrackRuntime objects are never
  // destroyed, so the pointer form needs no lock at all.
  // Every track's version advances: used where a change is NOT scoped to one track (a
  // project load replaces every clip; a waveform arrival invalidates every mirror), so
  // no caller is left holding a base that silently still matches.
  // Bumped whenever any track's sampler state changes, so a UI can poll one number instead of
  // re-requesting a kit to find out whether the one it drew is still current.
  std::atomic<uint32_t> samplerKitVersion{0};
  // One-shot: a generated event whose converted sample fell outside the block its TICK window
  // owns. Should be impossible; see the clamp that sets it.
  std::atomic<bool> warnedEventOutsideBlock{false};
  // One-shot: a device id too wide for the published half-word in UiPatcherNode.
  std::atomic<bool> warnedPatcherOwnerTooWide{false};
  std::atomic<uint32_t> chainVersion{0};
  std::atomic<uint32_t> routingVersion{0};
  std::atomic<uint32_t> modVersion{0};
  std::atomic<uint32_t> nextNoteId{1};
  std::atomic<uint32_t> nextChordId{1};
  // Monotonic stable placement id (published in placementId; the arrangement Move/Resize/
  // Remove key on it). Seeded above the max id loaded from a project so loaded + new ids
  // never collide. Assigned when a placement is created or loaded with id 0.
  std::atomic<uint32_t> nextPlacementId{1};
  // Seed the counter above any id already present in `placements`, then give every
  // unassigned (id == 0) placement a fresh stable id. Called wherever placements enter the
  // store (load, restore, single-note creation).
  std::mutex previewMutex;
  std::vector<PreviewNoteReq> pendingPreviewNotes;
  std::unordered_map<uint32_t, std::vector<uint8_t>> heldPreview;  // trackId -> held pitches
  // Enqueue an audition and update the held-pitch set. Caller holds nothing; this locks.
  auto enqueuePreview = [&](uint32_t trackId, uint8_t pitch, uint8_t velocity, bool on) {
    std::lock_guard<std::mutex> lock(previewMutex);
    pendingPreviewNotes.push_back({trackId, pitch, velocity, on});
    auto& held = heldPreview[trackId];
    const auto it = std::find(held.begin(), held.end(), pitch);
    if (on) {
      if (it == held.end()) held.push_back(pitch);
    } else if (it != held.end()) {
      held.erase(it);
    }
  };
  // The project's generation seed (ABI 4). Folded into every generator's hash so a song
  // reproduces exactly, and changing this one number re-rolls every generated variation.
  // 0 until a project supplies one.
  std::atomic<uint64_t> projectSeed{0};
  // PANIC (all sound off). The UI thread only raises this flag; the producer — the sole
  // writer of the per-track event rings — consumes it once per block and emits CC120 +
  // CC123 on every channel to every ready host, then drops that track's note state. Same
  // single-writer discipline as PreviewNote above.
  std::atomic<bool> panicPending{false};
  std::atomic<uint32_t> patcherGraphVersion{0};
  // Published so the UI can tell a failed LoadProject from a silent no-op:
  // projectLoadSeq bumps once per load attempt, projectLoadOk holds its result.
  std::atomic<uint32_t> projectLoadSeq{0};
  std::atomic<uint32_t> projectLoadOk{0};
  // Allocator for new/copy-on-write clip ids across all tracks' ownedClips.
  // Seeded past every loaded clip id so a fresh id never collides with a
  // retained one. Bumped when a track creates a clip or COW-forks a loaded one.
  std::atomic<uint32_t> nextClipId{1};
  std::mutex undoMutex;
  std::vector<EngineUndoEntry> undoStack;
  std::vector<EngineUndoEntry> redoStack;

  // Project-level clip definitions retained from load. Placements (per-track,
  // on TrackRuntime::sourcePlacements) reference these by id. Save re-emits the
  // ones still referenced by a clean track so the arrangement's structure
  // survives a load->save round-trip. Guarded by loadedClipsMutex.
  std::mutex loadedClipsMutex;
  std::vector<daw::ProjectClip> loadedClips;

  // Project tempo map retained from load so a save re-emits the FULL map (any tempo
  // changes included), rather than collapsing it to the current single tempo. Only
  // the load/save handlers touch it, and both run on the UI command thread, so it
  // needs no lock.

  // The song's time signature, adopted on load. Read on the audio callback (plugin
  // play head) and the publish thread (transport read-back), written on the UI thread
  // at load — relaxed atomics, since a meter one block stale is invisible.

  // Directory of the currently-loaded project file, so a clip's relative sourcePath
  // resolves against the project (portable) rather than the engine's CWD. Set by
  // loadProjectFromPath before the track loop; read by rebuildAudioRender.
  std::string loadedProjectDir;
  // history.jsonl (roadmap 19): an append-only journal of the commands this engine acted
  // on — {seq, ts_ms, author, scope, base_version, op, outcome, params}. Deliberately NOT
  // the DAW_EVENT telemetry stream: that is engine behaviour, this is "what was asked of
  // the document, in order", which is what makes it a crash-recovery and
  // what-changed-since-Tuesday artifact. NO INVERSES — reconstructing 32 correct inverses
  // plus schema-version replay is a project of its own; as a record it is nearly free.
  // Written from the command thread only (it does IO), guarded so a later multi-producer
  // ring cannot interleave half-lines.
  std::mutex historyMutex;
  uint64_t historySeq = 0;
  auto historyPath = [&]() -> std::filesystem::path {
    const std::string dir =
        loadedProjectDir.empty() ? daw::defaultProjectDir() : loadedProjectDir;
    return std::filesystem::path(dir) / "history.jsonl";
  };
  auto historyAppend = [&](const char* op, const char* outcome, uint32_t scopeTrack,
                           uint32_t baseVersion, const std::string& params) {
    if (std::getenv("DAW_NO_HISTORY")) {
      return;
    }
    std::lock_guard<std::mutex> lock(historyMutex);
    const auto path = historyPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::app);
    if (!out) {
      return;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    out << "{\"seq\":" << ++historySeq << ",\"ts_ms\":" << now
        << ",\"author\":\"ui\",\"scope\":";
    if (scopeTrack == 0xFFFFFFFFu) {
      out << "\"global\"";
    } else if (scopeTrack == daw::kMasterTrackId) {
      out << "\"master\"";
    } else {
      out << "\"track:" << scopeTrack << "\"";
    }
    out << ",\"base_version\":" << baseVersion << ",\"op\":\"" << op
        << "\",\"outcome\":\"" << outcome << "\",\"params\":{" << params << "}}\n";
  };
  // Engine-lifetime registry of decoded audio sources for waveform display: owns the
  // min/max pyramids the RequestWaveform handler slices, keyed by a stable sourceId.
  // Populated on the decode funnel (rebuildAudioRender), published to
  // UiAudioSourceRegion after a load. Read on the uiThread, never the RT callback.
  daw::WaveformStore waveformStore;

  // Need to grab these freshly after connect/reconnect
  auto getRingStd = [&](TrackRuntime& runtime) {
      return daw::makeEventRing(reinterpret_cast<void*>(
                                    const_cast<daw::ShmHeader*>(runtime.controller.shmHeader())),
                                runtime.controller.shmHeader()->ringStdOffset);
  };
  auto getRingCtrl = [&](TrackRuntime& runtime) {
      return daw::makeEventRing(reinterpret_cast<void*>(
                                     const_cast<daw::ShmHeader*>(runtime.controller.shmHeader())),
                                 runtime.controller.shmHeader()->ringCtrlOffset);
  };
  auto getRingUi = [&]() {
      if (!uiShm.header) {
        return daw::EventRingView{};
      }
      return daw::makeEventRing(uiShm.base, uiShm.header->ringUiOffset);
  };
  auto getRingUiAgent = [&]() {
      if (!uiShm.header || uiShm.header->ringUiAgentOffset == 0) {
        return daw::EventRingView{};
      }
      return daw::makeEventRing(uiShm.base, uiShm.header->ringUiAgentOffset);
  };
  auto getRingUiOut = [&]() {
      if (!uiShm.header) {
        return daw::EventRingView{};
      }
      return daw::makeEventRing(uiShm.base, uiShm.header->ringUiOutOffset);
  };
  const std::function<void(const char*, const char*, uint32_t, uint32_t,
                          const std::string&)> historyAppendFn = historyAppend;
  // MOVED UP TO HERE from beside the diff emitters, because UiPublishDeps now holds them and a
  // struct of references cannot be built before its members exist. They are four independent
  // declarations with nothing above them, so moving them is a move and not a reordering of work.
  std::atomic<uint64_t> uiDiffSent{0};
  std::atomic<uint64_t> uiDiffDropped{0};
  std::atomic<uint64_t> uiDiffDropLogMs{0};
  const auto uiDiffStart = std::chrono::steady_clock::now();
  daw::engine::UiPublishDeps uiPublishDeps{modVersion,          getRingStd,   getRingUiOut,
                                           routingVersion,      patcherGraphVersion,
                                           historyAppendFn,     uiDiffSent,   uiDiffDropped,
                                           uiDiffDropLogMs,     uiDiffStart};
  // The eight diff/error emitters are functions in engine_ui_publish now. main() keeps forwarders
  // because callers all through the file still use them by name.
  auto uiDiffNowMs = [&] { return daw::engine::uiDiffNowMs(uiPublishDeps); };
  // logUiDiffDrop has no forwarder: its only caller was sendUiDiff, which moved with it.
  auto sendUiDiff = [&](daw::EventRingView& ringUiOut, daw::EventType type,
                        const auto& diffPayload) {
    daw::engine::sendUiDiff(uiPublishDeps, ringUiOut, type, diffPayload);
  };
  auto emitUiDiff = [&](const daw::UiDiffPayload& diffPayload) {
    daw::engine::emitUiDiff(uiPublishDeps, diffPayload);
  };
  auto emitModError = [&](uint16_t errorCode, uint32_t trackId, uint32_t linkId) {
    daw::engine::emitModError(uiPublishDeps, errorCode, trackId, linkId);
  };
  auto emitRoutingError = [&](uint16_t errorCode, uint32_t trackId) {
    daw::engine::emitRoutingError(uiPublishDeps, errorCode, trackId);
  };
  auto emitClipReject = [&](daw::UiClipRejectReason reason, uint32_t trackId, uint32_t sentBase,
                            uint32_t currentBase, daw::UiCommandType commandType) {
    daw::engine::emitClipReject(uiPublishDeps, reason, trackId, sentBase, currentBase,
                                commandType);
  };
  auto reportSamplerReject = [&](daw::UiCommandType command, daw::UiSamplerRejectReason reason,
                                 uint32_t trackId, uint32_t deviceId, uint16_t targetId) {
    daw::engine::reportSamplerReject(uiPublishDeps, command, reason, trackId, deviceId, targetId);
  };
  auto emitModSnapshot = [&](TrackRuntime& runtime) {
    daw::engine::emitModSnapshot(uiPublishDeps, runtime);
  };
  auto writeMirrorParams = [&](TrackRuntime& runtime, const TrackStateSnapshot& trackState,
                               uint64_t sampleTime) {
    daw::engine::writeMirrorParams(uiPublishDeps, runtime, trackState, sampleTime);
  };
  auto emitRoutingSnapshot = [&](TrackRuntime& runtime) {
    daw::engine::emitRoutingSnapshot(uiPublishDeps, runtime);
  };
  auto emitPatcherGraphDelta = [&](uint32_t trackId, uint16_t flags, uint32_t nodeId,
                                   uint32_t nodeType, uint32_t srcNodeId, uint32_t dstNodeId,
                                   uint32_t srcPortId, uint32_t dstPortId, uint32_t edgeKind) {
    daw::engine::emitPatcherGraphDelta(uiPublishDeps, trackId, flags, nodeId, nodeType, srcNodeId,
                                       dstNodeId, srcPortId, dstPortId, edgeKind);
  };
  auto emitPatcherGraphError = [&](uint16_t errorCode, uint32_t trackId, uint32_t nodeId,
                                   uint32_t srcNodeId, uint32_t dstNodeId, uint32_t srcPortId,
                                   uint32_t dstPortId, uint32_t edgeKind) {
    daw::engine::emitPatcherGraphError(uiPublishDeps, errorCode, trackId, nodeId, srcNodeId,
                                       dstNodeId, srcPortId, dstPortId, edgeKind);
  };
  auto emitChainError = [&](uint16_t errorCode, uint32_t trackId, uint32_t deviceId,
                            uint32_t deviceKind, uint32_t insertIndex) {
    daw::engine::emitChainError(uiPublishDeps, errorCode, trackId, deviceId, deviceKind,
                                insertIndex);
  };
  auto getRingUiEdit = [&]() {
      if (!uiShm.header) {
        return daw::UiEditRingView{};
      }
      return daw::makeUiEditRing(uiShm.base, uiShm.header->ringUiEditOffset);
  };

  if (!uiTrack || getRingStd(*uiTrack).mask == 0 ||
      getRingCtrl(*uiTrack).mask == 0 || getRingUi().mask == 0 ||
      getRingUiOut().mask == 0) {
    daw::LogLine() << "Invalid ring capacity (must be power of two)." << std::endl;
    return 1;
  }

  auto snapshotTracks = [&]() {
    std::vector<TrackRuntime*> snapshot;
    std::lock_guard<std::mutex> lock(tracksMutex);
    snapshot.reserve(tracks.size());
    for (auto& runtime : tracks) {
      snapshot.push_back(runtime.get());
    }
    return snapshot;
  };

  // Below snapshotTracks, which it takes: a struct of references cannot be built before its
  // members exist, and snapshotTracksFn is the wrapper for the lambda just above.
  const std::function<std::vector<TrackRuntime*>()> snapshotTracksFn = snapshotTracks;
  daw::engine::SongExtentDeps songExtentDeps{transport, songTiming, snapshotTracksFn, patternTicks};
  auto trackWindowEnd = [&](const TrackRuntime& rt) {
    return daw::engine::trackWindowEnd(songExtentDeps, rt);
  };
  auto recomputeSongEnd = [&] { daw::engine::recomputeSongEnd(songExtentDeps); };
  // RE-ASSEMBLE THE PATCHER POOL FROM THE LIVE DEVICE GRAPHS.
  //
  // Each device owns an AUTHORED graph (device.patcher) with device-local node ids. The engine
  // runs ONE pool with globally-unique ids, built by offsetting each device's subgraph, and each
  // device's patcherNodeId is repointed at its own output node inside it. The authored graph is
  // the source of truth; the pool is derived — so this is idempotent and can be re-run after any
  // edit.
  //
  // Until now assembly happened ONLY at load, which is why editing a patcher graph at runtime did
  // nothing to what was executing (and, before the save guard, corrupted the file instead). This
  // is the same derivation the load performs, minus the document half: there is no document at
  // edit time, only runtimes, which makes it shorter rather than harder.
  //
  // Returns false when there is nothing to assemble or the pool will not build. A pool that will
  // not build is REPORTED and the previous one is left running — a bad edge in one device must not
  // silently take down every other device's graph.
  std::mutex clipWindowMutex;
  std::optional<ClipWindowPending> clipWindowPending;


  // v9: publish every track's clip in one region so read-only observers see
  // notes without the request ring. Rebuilt only when clipVersion moves — the
  // per-frame cost is otherwise a needless multi-megabyte memset. `force` seeds
  // the first publish and reruns after a load.
  uint32_t lastClipAllVersion = 0xFFFF'FFFFu;
  uint32_t lastClipAllQuantizeVersion = 0xFFFF'FFFFu;

  // v28: publish WHICH PARAMS ARE AUTOMATED — the standing lane list. Gated on
  // automationVersion, so a note edit does not rewrite it and a client can cache on the number.
  //
  // This exists because automation was writable and unreadable: nothing in the header mentioned
  // it, so the only lane a UI could offer was one you draw into and never see — blank while the
  // song plays the sweep you authored. The LIST alone makes lanes discoverable; the points are
  // answered on request (see the slot handler).
  //
  // The published `version` is this region's OWN GENERATION and starts at 1, so 0 means A WRITE IS
  // IN FLIGHT. Reading version-body-version and requiring the two to match is NOT torn-safe on its
  // own — the number only moves after the body is written, so a reader that samples it, reads a
  // body mid-rewrite, and samples again before the stamp sees v0 == v1 and accepts garbage. That
  // is the arrange summary's history verbatim, twenty lines below where this was first written;
  // the 0 sentinel is what actually makes the write visible while it is happening.
  uint32_t lastAutomationVersion = 0xFFFF'FFFFu;
  uint32_t automationGeneration = 0;

  // M3.25: publish the ARRANGEMENT SUMMARY — the section spine RESOLVED against the
  // meter, the meter points themselves, and the song end. Gated on sectionVersion so a
  // note edit does not rewrite it, and rebuilt whole rather than diffed: it is 4 KB and
  // a section reorder changes every entry anyway.
  // The region's published `version` is its OWN GENERATION, not the section version, and it
  // starts at 1 so that 0 can mean "a write is in flight" (see the stamping at the end).
  //
  // TWO THINGS THIS FIXES. First, the gate was the section version alone while the region also
  // carries songEndTick — and the song end changes on a PLACEMENT edit, which moves no section.
  // So a client that drew the song end from here kept the value from the last section edit, and
  // no reader could tell: the version it caches on had not moved either. Gating on both inputs
  // and publishing a generation means the number moves whenever anything in the region did.
  // A note edit still moves nothing, which is the property arrange_summary_check pins:
  // recomputeSongEnd runs only on a placement edit, a section ripple, or a load.
  //
  // Second, the torn read. The comments here used to claim that "reading version-body-version
  // and requiring the two to match is what makes a torn read impossible". That was wrong. The
  // version only changed AFTER the body was written, so a reader that sampled it, then read a
  // body mid-rewrite, then sampled again BEFORE the writer stamped, saw v0 == v1 and accepted
  // torn data. A seqlock needs the write to be visible while it is happening, which is what the
  // 0 sentinel below provides — the same odd/even trick the main ui_version already uses.
  uint32_t lastArrangeVersion = 0xFFFF'FFFFu;
  uint64_t lastArrangeSongEnd = 0xFFFF'FFFF'FFFF'FFFFull;
  uint32_t arrangeGeneration = 0;

  // M3.4: publish the placed-clip extents (rails). Rebuilt only when clipVersion
  // moves; loose placements are already excluded (they carry no runtime extent).
  uint32_t lastClipExtentVersion = 0xFFFF'FFFFu;
  daw::engine::ClipExtentsDeps clipExtentsDeps{
      clipVersion, lastClipExtentVersion, snapshotTracks, uiShm};

  auto writeUiClipExtents = [&](bool force) {
    daw::engine::writeUiClipExtents(clipExtentsDeps, force);
  };

  // v14: publish the patcher graph the engine runs, so the UI can draw it. Reads
  // the lock-free graph snapshot; only rewrites when the patcher version moves.
  uint32_t lastPatcherVersion = 0xFFFF'FFFFu;



  // The LOCK stays here and the RULE moved to apps/engine_rt_helpers.h. Splitting them is what
  // made the rule testable: a function that takes a mutex cannot be asked about its behaviour
  // without also arranging its concurrency.
  const auto& scaleRegistry = daw::ScaleRegistry::instance();

  // Binds the registry; the rule itself is in apps/engine_rt_helpers.h and has a unit test for
  // the unknown-scale fallback, which no fixture in tools/ exercises.
  auto quantizePitch = [&](uint8_t pitch,
                           const daw::HarmonyEvent& harmony) -> daw::ResolvedPitch {
    return daw::engine::quantizePitch(scaleRegistry, pitch, harmony);
  };


  std::atomic<uint64_t> lastOverflowTick{0};
  std::atomic<bool> running{true};
  std::atomic<uint32_t> nextBlockId{1};
  
  auto resolvePluginPath = [&](uint32_t pluginIndex) -> std::optional<std::string> {
    if (pluginIndex >= pluginCache.entries.size()) {
      return std::nullopt;
    }
    const auto& entry = pluginCache.entries[pluginIndex];
    if (entry.scanStatus != daw::ScanStatus::Ok && !entry.error.empty()) {
      return std::nullopt;
    }
    return entry.path;
  };

  auto resolveDevicePluginPath =
      [&](const TrackRuntime& runtime,
          uint32_t hostSlotIndex) -> std::optional<std::string> {
    if (hostSlotIndex == daw::kHostSlotIndexDirect) {
      // "Direct" means the engine's default plugin. Resolve it from the STABLE
      // baseConfig, not runtime.config.pluginPaths — the latter is overwritten by every
      // rebuildHostForChain, so a Direct device would otherwise inherit whatever plugin
      // the previously loaded project left behind (a multi-out project opened after a
      // real-plugin project loaded the wrong instance and produced no stems). Real
      // projects pin devices to a cache index, so this branch is the test/default path.
      if (!baseConfig.pluginPaths.empty()) {
        return baseConfig.pluginPaths.front();
      }
      if (!runtime.config.pluginPaths.empty()) {
        return runtime.config.pluginPaths.front();
      }
      return std::nullopt;
    }
    return resolvePluginPath(hostSlotIndex);
  };

  auto applyHostBypassStates = [&](TrackRuntime& runtime) {
    if (!runtime.hostReady.load(std::memory_order_acquire)) {
      return;
    }
    std::vector<daw::Device> devices;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      devices = runtime.track.chain.devices;
    }
    uint32_t hostIndex = 0;
    std::lock_guard<std::mutex> lock(runtime.controllerMutex);
    for (const auto& device : devices) {
      if (device.kind != daw::DeviceKind::VstInstrument &&
          device.kind != daw::DeviceKind::VstEffect) {
        continue;
      }
      runtime.controller.sendSetBypass(hostIndex, device.bypass);
      hostIndex++;
    }
  };

  daw::engine::ChainHostDeps chainHostDeps{
      applyHostBypassStates, resolveDevicePluginPath};

  auto rebuildHostForChain = [&](TrackRuntime& runtime) {
    daw::engine::rebuildHostForChain(chainHostDeps, runtime);
  };

  // Movement 4 multi-out: a hostless CHILD track, built as an ordinary runtime (buffers
  // and all, so every all-tracks loop stays safe) but with an empty chain and no host.
  // It carries the aux-view fields that point the mixer at bus `busIndex` of the parent's
  // aux output plane. Appended to `tracks` at a contiguous id by reconcileChildTracks.
  auto setupAuxChildRuntime = [&](uint32_t childId, uint32_t parentTrackId,
                                  uint32_t busIndex, uint32_t busChannelOffset,
                                  uint32_t busChannelCount,
                                  const std::string& name)
      -> std::unique_ptr<TrackRuntime> {
    auto runtime = setupTrackRuntime(childId, "", false, false);
    if (!runtime) {
      return nullptr;
    }
    runtime->track.chain = daw::TrackChain{};  // no plugins
    runtime->trackSnapshot = buildTrackSnapshot(runtime->track);
    runtime->trackName = name;
    runtime->parentId.store(parentTrackId, std::memory_order_relaxed);
    runtime->collapsed.store(false, std::memory_order_relaxed);
    runtime->isAuxChild.store(true, std::memory_order_release);
    runtime->auxParentTrackId.store(parentTrackId, std::memory_order_relaxed);
    runtime->auxBusIndex.store(busIndex, std::memory_order_relaxed);
    runtime->auxBusChannelOffset.store(busChannelOffset, std::memory_order_relaxed);
    runtime->auxBusChannelCount.store(busChannelCount, std::memory_order_relaxed);
    return runtime;
  };

  // Movement 4 multi-out: (re)derive child tracks for a parent whose plugin splits its
  // outputs. Queries the flagged plugin's negotiated bus layout, then for each enabled
  // aux OUTPUT bus ensures a child runtime exists (idempotent — never duplicates on a
  // re-run). Child audio is a view into that bus's slice of the parent's aux plane; the
  // aux plane offset of bus B is its plugin channelOffset minus the main width. Removal
  // of children when a plugin is unloaded is a later refinement; today they persist and
  // read silence once the parent stops writing that bus.
  daw::engine::ChildTrackDeps childTrackDeps{
      baseConfig, buildTrackSnapshot, clipVersion, liveTrackCount, resetTrackContent,
      setupAuxChildRuntime, trackTable};

  auto reconcileChildTracks = [&](TrackRuntime& parent) {
    daw::engine::reconcileChildTracks(childTrackDeps, parent);
  };

  auto scheduleHostRestart = [&](TrackRuntime& runtime) {
    // Movement 4: an aux child has no host to (re)start.
    if (runtime.isAuxChild.load(std::memory_order_acquire)) {
      return;
    }
    // A track we've given up on stays dead until the chain is rebuilt; don't
    // re-arm the restart loop for it.
    if (runtime.hostGaveUp.load(std::memory_order_acquire)) {
      return;
    }
    bool expected = false;
    if (!runtime.restartInFlight.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return;
    }
    runtime.hostReady.store(false, std::memory_order_release);
    runtime.active.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(restartMutex);
      restartQueue.push_back(&runtime);
    }
    restartCv.notify_one();
  };



  // BUILT HERE, not beside trackSetupDeps at the top: rebuildHostForChain and scheduleHostRestart
  // are declared just above, and a struct of references cannot be built before its members exist.
  // See TrackLifecycleDeps for why that forces two structs rather than one.
  const std::function<void(TrackRuntime&)> rebuildHostForChainFn = rebuildHostForChain;
  const std::function<void(TrackRuntime&)> scheduleHostRestartFn = scheduleHostRestart;
  daw::engine::TrackLifecycleDeps trackLifecycleDeps{
      trackSetupDeps, trackTable, masterTrack, liveTrackCount, masterFxActive,
      rebuildHostForChainFn, scheduleHostRestartFn};
  auto ensureTrack = [&](uint32_t trackId, const std::string& pluginPath) {
    return daw::engine::ensureTrack(trackLifecycleDeps, trackId, pluginPath);
  };
  auto restartTrackHost = [&](TrackRuntime& runtime,
                              const std::vector<std::string>& pluginPaths) {
    return daw::engine::restartTrackHost(trackLifecycleDeps, runtime, pluginPaths);
  };
  auto reconcileMasterHost = [&] { daw::engine::reconcileMasterHost(trackLifecycleDeps); };
  // AT main() SCOPE, NOT BESIDE THE std::thread. masterRenderThread is started inside a nested
  // block and joined at the end of main(), so both this struct and the std::function it holds a
  // reference to must live at least that long. The first version of this extraction declared them
  // in the inner block and ALSO shadowed the outer std::thread with a local one — the thread never
  // started, and what did start read a destroyed struct. 100 checks failed.
  daw::engine::MasterRenderDeps masterRenderDeps{
      running, transport, masterFxActive, masterTrack, audioCallback, scheduleHostRestartFn
  };
  // 4b: bring the MASTER host in line with its chain. The master is not in the `tracks`
  // vector, so the per-track consumer never drives its host lifecycle — do it here,
  // off the command/load thread. rebuildHostForChain resolves the master's VST paths and
  // either reconciles a running host in place or arms needsRestart; the restart worker
  // (which operates on any runtime, not just tracks) then launches it. A master with only
  // patcher/mod devices resolves to no plugins, so no host is launched. The master render
  // thread (below) drives its blocks once it is ready.
  daw::engine::RestartWorkerDeps restartWorkerDeps{
      running, restartMutex, restartCv, restartQueue, applyHostBypassStates};
  std::thread restartWorker([&] { daw::engine::runRestartWorker(restartWorkerDeps); });

  auto updateTrackChainForInstrument = [&](TrackRuntime& runtime,
                                           uint32_t pluginIndex) {
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      auto& devices = runtime.track.chain.devices;
      auto it = std::find_if(devices.begin(), devices.end(),
                             [&](const daw::Device& device) {
                               return device.kind == daw::DeviceKind::VstInstrument;
                             });
      if (it == devices.end()) {
        const daw::Device instrument =
            daw::makeVstInstrumentDevice(pluginIndex);
        daw::addDevice(runtime.track.chain, instrument, daw::kDeviceIdAuto);
      } else {
        it->hostSlotIndex = pluginIndex;
        it->capabilityMask =
            daw::capabilityMaskForKind(daw::DeviceKind::VstInstrument);
      }
    }
    rebuildHostForChain(runtime);
  };

  // EVERY DIFF SEND IS COUNTED, AND A DROP IS LOGGED — once, instead of in three emitters.
  //
  // emitUiDiff, emitHarmonyDiff and emitChordDiff are siblings for three EventTypes, and each
  // carried its own twelve-line copy of the send. They are not copy-paste laziness: each was
  // written deliberately for its own payload. What nobody wrote down is that they have to stay in
  // step, and a fourth emitter that forgot the counters would not look like a bug — it would look
  // like a quieter engine, because uiDiffSent/uiDiffDropped is exactly what the drop telemetry
  // reports.
  //
  // A generic lambda rather than a template function: it needs ringUiOut, both counters and
  // logUiDiffDrop, all of which are main's locals. The size comes from the payload's own type, so
  // the declared size and the copied bytes cannot disagree.
  // The ring is a PARAMETER because each emitter obtains its own view with getRingUiOut() and
  // returns early if the mask is zero. Capturing a ring here would have meant one of them using a
  // view the caller had already decided not to write to.
  // EVERY SAMPLER REFUSAL REACHES THE CALLER, not just the engine's log.
  //
  // Twenty sites across seven sampler commands reported refusal with DAW_EVENT and nothing else.
  // daw-cli can read stderr; a browser cannot. So from a UI every one of them was a silent no-op
  // that reported success — the web-UI agent sent SamplerSetSlot with slot 0, got `no_such_slot`
  // in a log they never see, and watched the command succeed while the sound ran to its end.
  //
  // The rule is PresetSaved's: every exit reports, including the early refusals, because a caller
  // that gets nothing back cannot tell "refused" from "still working" from "done".
  //
  // The DAW_EVENT lines stay. They are how a human and daw-cli read it, and the two carry the
  // same facts because this is called beside them rather than instead of them.
  // A refusal, on the outbound ring, with the numbers that settle it. Everything the
  // caller needs to recover is here: which track the version was compared against, what
  // it sent, and what to retry with.
  const std::function<void(daw::UiClipRejectReason, uint32_t, uint32_t, uint32_t,
                           daw::UiCommandType)> emitClipRejectFn = emitClipReject;

  daw::engine::ChainSnapshotDeps chainSnapshotDeps{
      chainVersion, getRingUiOut, resolveDevicePluginPath};

  auto emitChainSnapshot = [&](TrackRuntime& runtime) {
    daw::engine::emitChainSnapshot(chainSnapshotDeps, runtime);
  };

  // A refusal has to reach somewhere a PERSON can read. These three emitters wrote only
  // to the outbound ring, so a refused routing/chain/mod command left no trace in the
  // engine log and no entry in history.jsonl — a script or an agent saw "sent" and
  // nothing else. That is the same silent-failure shape that cost the frontend an
  // afternoon on stale clip versions, and it applies to every CLI path added for these
  // ops. So: the diff still goes on the ring for the UI, and the same refusal is now
  // also an event and a journal line.

  auto emitHarmonyDiff = [&](const daw::UiHarmonyDiffPayload& diffPayload) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    sendUiDiff(ringUiOut, daw::EventType::UiHarmonyDiff, diffPayload);
  };

  auto emitChordDiff = [&](const daw::UiChordDiffPayload& diffPayload) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    sendUiDiff(ringUiOut, daw::EventType::UiChordDiff, diffPayload);
  };

  auto pushUndo = [&](EngineUndoEntry entry) {
    std::lock_guard<std::mutex> lock(undoMutex);
    undoStack.push_back(std::move(entry));
    redoStack.clear();
  };

  // Harmony edits keep their absolute-tick undo, wrapped as a non-structural entry
  // so they share one heterogeneous undo stack with structural store swaps.
  auto pushHarmonyUndo = [&](const daw::UndoEntry& undo) {
    EngineUndoEntry e;
    e.structural = false;
    e.trackId = undo.trackId;
    e.harmony = undo;
    pushUndo(std::move(e));
  };


  // THE HARMONY TIMELINE OWNS ITS OWN STATE NOW. These four used to be four separate locals of
  // main() and are still spelled the same, so every reader below is unchanged — the bindings are
  // what keep this commit a move rather than a rewrite. The *Deps structs that carry them
  // individually (17 members across six structs) can collapse to one HarmonyTimeline& each next.
  daw::engine::HarmonyTimeline harmonyTimeline{scaleRegistry, emitHarmonyDiff, pushHarmonyUndo};
  // NO ALIASES INTO HarmonyTimeline REMAIN. harmonyDirty, harmonyMutex, harmonyEvents and
  // harmonyVersion each had a binding here purely so older code could keep spelling them. Their
  // last readers in main() were the song-store functions and the version guard, and both moved to
  // where the state lives. This is the engine-object payoff running in the intended direction:
  // main() sheds locals as the code that used them leaves, instead of accumulating aliases.
  const std::function<std::optional<daw::HarmonyEvent>(uint64_t)> getHarmonyAt =
      [&](uint64_t nanotick) { return harmonyTimeline.getHarmonyAt(nanotick); };
  const std::function<const daw::Scale*(const daw::HarmonyEvent&)> getScaleForHarmony =
      [&](const daw::HarmonyEvent& h) { return harmonyTimeline.getScaleForHarmony(h); };
  const std::function<bool(uint64_t, uint32_t, uint32_t, bool)> addOrUpdateHarmony =
      [&](uint64_t t, uint32_t r, uint32_t sc, bool u) {
        return harmonyTimeline.addOrUpdateHarmony(t, r, sc, u);
      };
  const std::function<bool(uint64_t, bool)> removeHarmony =
      [&](uint64_t t, bool u) { return harmonyTimeline.removeHarmony(t, u); };

  // Plugin state sits in a sibling directory rather than inside the JSON:
  // blobs are opaque and often large, and keeping them out keeps the document
  // diffable. The container shape (this, or the zip PROJECT_PERSISTENCE.md
  // describes) is still an open decision.

  // The song's end: the furthest placement end across every LIVE track. Runs on the
  // command thread after any placement edit, never on the audio thread.
  //
  // The loop follows it ONLY while the user has not set a loop by hand. Both halves
  // matter: without the follow, material added past the old end is silent forever;
  // without the guard, every placement edit would quietly discard a loop the user chose,
  // which is the same bug pointing the other way.
  // The flatten window for a track: past every placement's resolved end, at least
  // one pattern bar. Used so a note stretched or looped past the old arrangement
  // end still lands in the derived flat clip.
  // The single funnel for the structural note store: re-derive track.clip and the
  // rail extents from (sourcePlacements + ownedClips) and return a fresh snapshot.
  // Assumes runtime->trackMutex is held; the caller atomic_stores the returned
  // snapshot after unlocking. The audio thread reads only the snapshot, so
  // track.clip being derived is invisible to it.

  daw::engine::FlatRebuildDeps flatRebuildDeps{
      laneQuantizeOf, trackWindowEnd};

  auto rebuildFlatAndPublish = [&](TrackRuntime& rt)
      -> std::shared_ptr<const ClipSnapshot> {
    return daw::engine::rebuildFlatAndPublish(flatRebuildDeps, rt);
  };

  // Resolve a clip's sourcePath the one way both the decode funnel and the clip-
  // descriptor publish must agree on: absolute paths as given; relative paths against
  // the project directory; then fold '..'/symlinks so one file yields one stable key.
  // REGISTER A DECODED FILE FOR WAVEFORM DISPLAY — one definition, two callers.
  //
  // `decodeAudioFile` already BUILDS the min/max pyramid; the clip path interned it and the
  // sampler path decoded the same way and dropped it on the floor. So a sampler's audio existed,
  // was drawable, and had no entry in the store — which is why RequestWaveform could not answer
  // for a pad no matter what id was sent (the web-UI agent found it from the outside: the request
  // went out, no window ever landed, and the model was perfect throughout, which is exactly what
  // a source that failed to decode looks like from there).
  //
  // KEYED BY RESOLVED PATH, which is the property worth keeping: a break loaded into a sampler
  // AND placed as an audio clip is ONE entry and ONE pyramid. The content key folds in size and
  // mtime, so a file re-bounced in place invalidates rather than serving a stale picture.
  auto internDecodedForWaveform = [&](const std::string& resolvedPath,
                                      const daw::DecodedAudio& dec) -> uint32_t {
    uint64_t fileSize = 0, mtimeNs = 0;
    std::error_code sec;
    auto sz = std::filesystem::file_size(resolvedPath, sec);
    if (!sec) fileSize = static_cast<uint64_t>(sz);
    std::error_code tec;
    auto ft = std::filesystem::last_write_time(resolvedPath, tec);
    if (!tec) {
      mtimeNs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(ft.time_since_epoch()).count());
    }
    const uint64_t contentKey = daw::computeWaveformContentKey(
        resolvedPath, fileSize, mtimeNs, dec.frames, dec.sampleRate, dec.sourceChannels,
        daw::kDecoderVersion, daw::kWaveformFormatVersion);
    const auto& py = dec.pyramid;
    return waveformStore.internReady(resolvedPath, contentKey, dec.sourceChannels, dec.frames,
                                     dec.sampleRate, py ? py->absPeak : 0.0f,
                                     py ? py->levelMask : 0u, py && py->channelsTruncated,
                                     py && py->clipped, py);
  };

  // THE SAMPLER'S SNAPSHOT. Decodes every source the device names and flattens the document into
  // the immutable form the producer thread reads (docs/SAMPLER_DESIGN.md §3.5).
  //
  // Runs OFF the audio path — it opens files — and the result is handed over by
  // atomic_store_explicit, exactly as trackSnapshot and audioRender already are. The snapshot
  // OWNS its audio by shared_ptr, so a render in flight keeps its buffers alive by construction
  // and the last reference dies here, on the command thread, where a free is legal.
  // Both moved into engine_sampler_commands as free functions. main() keeps forwarders because
  // it still calls them; the module itself reaches them directly, without a std::function.
  auto resolveSourcePath = [&](const std::string& sourcePath) {
    return daw::engine::resolveSourcePath(loadedProjectDir, sourcePath);
  };

  auto rebuildSamplerRender =
      [&](const daw::SamplerState& st,
          uint32_t trackId,
          uint32_t deviceId) -> std::shared_ptr<const daw::SamplerRender> {
    auto out = std::make_shared<daw::SamplerRender>();
    out->state = st;
    out->sampleRate = engineConfig.sampleRate;
    out->keymap.rebuild(out->state);
    uint32_t decoded = 0, failed = 0, changed = 0;
    for (const auto& src : st.sources) {
      const std::string path = resolveSourcePath(src.path);
      daw::DecodedAudio dec = daw::decodeAudioFile(path);
      if (!dec.ok || dec.channels.empty() || dec.frames == 0) {
        // NEVER A QUIET SUBSTITUTION. A missing sample leaves a null entry, so the slot is
        // SILENT and says so — loading "something else" is the kHostSlotIndexUnresolved failure,
        // where every structural check passes and only the audio is wrong.
        out->audio.push_back(nullptr);
        ++failed;
        DAW_EVENT("sampler.source_missing")
            .field("track", trackId)
            .field("device", deviceId)
            .field("source", static_cast<uint32_t>(src.localId))
            .field("path", path);
        continue;
      }
      // REGISTERED FOR DISPLAY BEFORE THE CHANNELS ARE MOVED OUT — the pyramid rides on `dec`
      // and this is the last moment it is whole. Without this a pad's audio plays and cannot be
      // DRAWN: the sample view had every extent it needed and no waveform to put them on.
      internDecodedForWaveform(path, dec);
      auto audio = std::make_shared<daw::SamplerSourceAudio>();
      audio->channels = std::move(dec.channels);
      audio->frames = dec.frames;
      audio->sampleRate = dec.sampleRate;
      audio->buildPlanes();
      out->audio.push_back(std::move(audio));
      ++decoded;
      // The content key is ADVISORY: recomputed here so a changed file is REPORTED, never so the
      // stored value can be trusted. Loud difference beats quiet substitution for audio.
      if (src.contentKey != 0) {
        uint64_t fileSize = 0, mtimeNs = 0;
        std::error_code sec;
        auto sz = std::filesystem::file_size(path, sec);
        if (!sec) fileSize = static_cast<uint64_t>(sz);
        std::error_code tec;
        auto ft = std::filesystem::last_write_time(path, tec);
        if (!tec) {
          mtimeNs = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(ft.time_since_epoch())
                  .count());
        }
        const uint64_t now = daw::computeWaveformContentKey(
            path, fileSize, mtimeNs, dec.frames, dec.sampleRate, dec.sourceChannels,
            daw::kDecoderVersion, daw::kWaveformFormatVersion);
        if (now != 0 && now != src.contentKey) {
          ++changed;
          DAW_EVENT("sampler.source_changed")
              .field("track", trackId)
              .field("device", deviceId)
              .field("source", static_cast<uint32_t>(src.localId))
              .field("path", path)
              .field("saved_key", src.contentKey)
              .field("current_key", now);
        }
      }
    }
    DAW_EVENT("sampler.render_built")
        .field("track", trackId)
        .field("device", deviceId)
        .field("slots", static_cast<uint32_t>(st.slots.size()))
        .field("decoded", decoded)
        .field("failed", failed)
        .field("changed", changed);
    return out;
  };

  const std::function<std::shared_ptr<const daw::SamplerRender>(
      const daw::SamplerState&, uint32_t, uint32_t)> rebuildSamplerRenderFn =
      rebuildSamplerRender;
  daw::engine::SamplerRefreshDeps samplerRefreshDeps{
      engineConfig, samplerKitVersion, rebuildSamplerRenderFn};
  auto refreshSamplerForTrack = [&](TrackRuntime& rt) {
    daw::engine::refreshSamplerForTrack(samplerRefreshDeps, rt);
  };

  // Installs (or clears) a track's sampler from its device chain. Called from EVERY site that
  // changes a chain, so "did you remember to rebuild the sampler" is not a question anyone has to
  // answer twice. Caller holds trackMutex.
  // Resolve a track's placed AUDIO clips into a sample-domain render list for the
  // audio thread: decode each source (deduped per rebuild), and convert its
  // placement to output frames. Runs off the audio thread (decodes files); the caller
  // atomic_stores the result into rt.audioRender. Assumes trackMutex is held for the
  // store reads.
  //
  // M3.22: positions are ABSOLUTE, so they are integrated over the tempo map rather
  // than multiplied by one tempo. This used to take bpmAtNanotick(0) and apply it to
  // every tick in the project, which treats the whole song as though it had never
  // changed tempo — with a change at bar 3, an audio clip at bar 9 landed at the wrong
  // sample, and the further into the song the worse it got. The note scheduler was
  // always fine: it advances tick by tick per block using the LOCAL tempo, which is a
  // different (and also correct) computation.
  daw::engine::AudioRenderRebuildDeps audioRenderRebuildDeps{
      engineConfig, internDecodedForWaveform, resolveSourcePath, tickConverter, waveformStore};

  auto rebuildAudioRender = [&](const TrackRuntime& rt)
      -> std::shared_ptr<const AudioRenderList> {
    return daw::engine::rebuildAudioRender(audioRenderRebuildDeps, rt);
  };

  // PUBLISH THE AUDIO CLIP DESCRIPTOR TABLE (contract §2.1) and bump the region version.
  //
  // This used to be a loop inlined in loadProjectFromPath, reading that function's local
  // `document`, under a comment saying "these change only at load, so no seqlock". True until
  // SetAudioClipField (95) existed; the moment a command can move a clip's gain or fades, a
  // table published once at load is the opcode 94 defect in a second table — written, saved,
  // honoured by the renderer, and never seen by anyone reading the shared memory.
  //
  // SOURCED FROM THE LIVE PER-TRACK STORE FIRST. runtime->ownedClips is what the renderer reads
  // and what a save re-emits, so it is the authority; the load-time `document` was a copy that
  // stopped tracking edits the instant it was made. Retained definitions that no placement
  // references are appended from `loadedClips` afterwards, because those exist only there and
  // dropping them would be a regression in what the table lists.
  //
  // Deduped by clip id across tracks: a child track's ownedClips is a copy of its parent's
  // (see the aux-plane overlay), so the same clip is reachable from two runtimes and would
  // otherwise be published twice and eat the 64-entry budget.
  //
  // LOCK ORDER is tracksMutex -> trackMutex, taken as a pointer snapshot under tracksMutex and
  // then released, matching every other command-thread walk over all tracks.
  daw::engine::AudioClipTableDeps audioClipTableDeps{
      loadedClips, loadedClipsMutex, resolveSourcePath, tempoProvider, trackTable, uiShm,
      waveformStore};

  auto publishAudioClipTable = [&]() {
    daw::engine::publishAudioClipTable(audioClipTableDeps);
  };


  // Locate the owned clip a structural edit at absTick belongs to, via the shared
  // resolveNoteEntry rule. For an add (createIfMissing), CreateNew allocates a new
  // empty clip+placement anchored to the bar; for a remove it returns
  // {valid=false} instead (nothing outside a clip to remove). Does NOT copy-on-
  // write fork — the caller forks (forkOwnedClip) only after an edit that actually
  // changed the clip, so a no-op remove never churns clip ids. Assumes trackMutex
  // is held.
  daw::engine::LocateTargetDeps locateTargetDeps{
      nextClipId, nextPlacementId, songBarGrid};

  auto locateEditTarget = [&](TrackRuntime& rt, uint64_t absTick,
                              bool createIfMissing) -> EditTarget {
    return daw::engine::locateEditTarget(locateTargetDeps, rt, absTick, createIfMissing);
  };

  // Copy-on-write: after an edit that changed a pristine (still-shared) loaded
  // clip, give it a fresh id and repoint this track's placements, so save never
  // emits two divergent clips under one id. No-op once the clip is track-owned.
  // Grow the target clip's loop length (and any explicit placement length) to
  // contain its content after an edit, so the flatten's "beyond clip length" guard
  // never drops a just-stretched note. A linear length-0 clip stays 0 (it plays
  // once, no loop, so nothing is dropped and nothing needs growing).
  auto snapshotTrackStore = [&](const TrackRuntime& rt) -> TrackStoreState {
    TrackStoreState s;
    s.placements = rt.sourcePlacements;
    s.clips = rt.ownedClips;
    s.editable = rt.editableClipIds;
    return s;
  };

  // The whole song's structural state, for a section ripple's before/after. Takes each track's
  // mutex in turn rather than all at once — this runs on the control thread with no other lock
  // held, and holding every trackMutex simultaneously is how a lock-order inversion gets written.
  auto pushStructuralUndo = [&](uint32_t trackId, TrackStoreState before,
                                TrackStoreState after) {
    EngineUndoEntry e;
    e.structural = true;
    e.trackId = trackId;
    e.before = std::move(before);
    e.after = std::move(after);
    pushUndo(std::move(e));
  };

  // EVERY STRUCTURAL EDIT DOES THESE THREE THINGS, and it was written out five times.
  //
  // Mark the arrangement dirty, republish the flat clip, record the undo entry. Missing any one
  // fails silently and differently: no pushStructuralUndo and undo skips the edit; no
  // arrangementDirty and the UI keeps drawing the old arrangement; no rebuildFlatAndPublish and
  // the audio thread plays a snapshot that no longer matches the store. Five copies meant a sixth
  // structural edit could get two of the three right and look entirely correct.
  //
  // THIS IS NOT EVERY CALLER OF THOSE THREE. The file has 13 arrangementDirty stores, 22
  // rebuildFlatAndPublish calls and 10 pushStructuralUndo calls: plenty of edits legitimately do a
  // subset — a non-structural change needs the republish and the dirty flag but records no undo
  // entry of its own. Only the five that do all three in this order are collapsed here; the rest
  // are different operations, not sloppy copies of this one.
  //
  // Stays a lambda rather than moving to a module: it needs rebuildFlatAndPublish,
  // pushStructuralUndo and snapshotTrackStore, all still main's lambdas. Extracting it would mean
  // a struct of three std::function members to carry three calls — the dispatch-shell shape, which
  // moves lines without moving behaviour.
  auto commitStructuralEdit = [&](TrackRuntime& rt, uint32_t tid, TrackStoreState&& before,
                                  bool recordUndo) -> std::shared_ptr<const ClipSnapshot> {
    rt.arrangementDirty.store(true, std::memory_order_relaxed);
    auto snap = rebuildFlatAndPublish(rt);
    if (recordUndo) {
      pushStructuralUndo(tid, std::move(before), snapshotTrackStore(rt));
    }
    return snap;
  };

  // Tell an incremental UI to pull a fresh clip window after a whole-store change
  // it cannot diff note-by-note (an undo/redo store swap).
  auto emitClipResync = [&](uint32_t trackId, uint32_t clipVersionValue) {
    daw::UiDiffPayload diff{};
    diff.diffType = static_cast<uint16_t>(daw::UiDiffType::ResyncNeeded);
    diff.trackId = trackId;
    diff.clipVersion = clipVersionValue;
    emitUiDiff(diff);
  };

  // Restore a track's structural store (placements + owned clips + editable ids)
  // to a captured state and re-derive/publish the flat clip. The engine-local undo
  // stack's structural entries call this with `before` (undo) or `after` (redo).
  // Put the whole song back. Everything the ripple touched, or the restore is partial — and a
  // partial restore of a ripple is worse than none: the placements would be back where they were
  // while the tempo change and the filter sweep stayed at their new positions.
  // Snapshots the live session into a ProjectDocument and writes it. Each
  // track is copied under its own mutex so the document is consistent per
  // track without stalling audio behind one global lock.
  daw::engine::SaveProjectDeps saveProjectDeps{
      arrange, harmonyTimeline, liveTrackCount, loadedClips, loadedClipsMutex, songTiming,
      masterTrack, patcherGraph, pluginCache, projectSeed, trackTable, songBarGrid,
      trackIsPersisted
  };
  auto saveProjectToPath = [&](const std::string& path,
                                 std::string* error) -> bool {
    return daw::engine::saveProjectToPath(saveProjectDeps, path, error);
  };

  daw::engine::ClipEditDeps clipEditDeps{
      barEndTick, clipDirty, clipVersion, nextPlacementId, commitStructuralEdit,
      emitChordDiff, emitUiDiff,
      locateEditTarget, transport, nextChordId,
      nextClipId, patternTicks, pushStructuralUndo, rebuildFlatAndPublish,
      snapshotTrackStore, trackTable, emitClipRejectFn, historyAppendFn
  };
  // Six helpers that used to be lambdas here are functions in engine_clip_edit now — three of
  // them were MEMBERS of the struct above, so it lost three std::functions and gained one
  // reference. main() keeps forwarders because callers further down still use them by name.
  auto bumpClipVersionFor = [&](TrackRuntime* runtime) {
    return daw::engine::bumpClipVersionFor(clipEditDeps, runtime);
  };
  auto bumpAllTrackClipVersions = [&] {
    daw::engine::bumpAllTrackClipVersions(clipEditDeps);
  };
  auto ensurePlacementIds = [&](std::vector<daw::ProjectPlacement>& placements) {
    daw::engine::ensurePlacementIds(clipEditDeps, placements);
  };
  auto editIsLocalScope = [&](uint32_t trackId, uint64_t nanotick, uint16_t flags) {
    return daw::engine::editIsLocalScope(clipEditDeps, trackId, nanotick, flags);
  };

  // Restores the musical document: clips, harmony and per-track harmony
  // quantize. Device chains and plugin state are intentionally not reapplied
  // here — that needs host restarts and the vst_state blobs described in
  // PROJECT_PERSISTENCE.md, which this version does not yet write.
  daw::engine::LoadProjectDeps loadProjectDeps{
      arrange, automationVersion, auxChildOverlayMutex, auxChildOverlays,
      buildTrackSnapshot, bumpAllTrackClipVersions, clipDirty, clipVersion,
      emitChainSnapshot, emitModSnapshot, emitRoutingSnapshot, emitUiDiff,
      ensurePlacementIds, ensureTrack, harmonyTimeline, liveTrackCount, loadInProgress,
      loadedClips, loadedClipsMutex, loadedProjectDir, songTiming, transport, masterTrack,
      nextClipId, patcherGraph, patternTicks, pluginCache, projectSeed,
      publishAudioClipTable, rebuildAudioRender, rebuildFlatAndPublish, rebuildHostForChain,
      reconcileMasterHost, refreshSamplerForTrack, resetTrackContent, tempoProvider, trackTable, updatePatcherGraphSnapshot, waveformStore
  };

  auto loadProjectFromPath = [&](const std::string& path, std::string* error) -> bool {
    return daw::engine::loadProjectFromPath(loadProjectDeps, path, error);
  };

  // The UI reserves one clip version per edit it queues, so an edit whose
  // base version matched must advance the counter even when the edit turns out
  // to be a no-op. Otherwise the UI stays permanently one ahead and every
  // later edit is rejected — inside a batch that discards the whole remainder
  // and emits a resync request per op.
  // M2.17: acceptance is PER TRACK. The caller presents the version of the track it is
  // editing (published in uiTrackClipVersion), so an edit to track 4 is no longer refused
  // because someone typed on track 1 — the collision that made `daw-cli do` need --force
  // and made two authors impossible. Falls back to the global counter when the track is
  // unknown, which keeps non-track-scoped edits behaving exactly as before.
  auto requireMatchingHarmonyVersion = [&](uint32_t baseVersion,
                                          daw::UiCommandType commandType) {
    return harmonyTimeline.requireMatchingHarmonyVersion(baseVersion, commandType);
  };


  auto requireMatchingClipVersion = [&](uint32_t baseVersion, daw::UiCommandType commandType, uint32_t trackId) {
    return daw::engine::requireMatchingClipVersion(clipEditDeps, baseVersion, commandType, trackId);
  };
  auto findPlacementAt = [&](TrackRuntime& rt, uint64_t nanotick) {
    return daw::engine::findPlacementAt(clipEditDeps, rt, nanotick);
  };

  auto applyAddNote = [&](uint32_t trackId, uint64_t nanotick, uint64_t duration,
                          uint8_t pitch, uint8_t velocity, uint16_t flags, bool recordUndo,
                          std::optional<daw::EventId> noteIdOverride = std::nullopt,
                          uint16_t sound = 0, uint16_t soundOffset = 0) -> bool {
    return daw::engine::applyAddNote(clipEditDeps, trackId, nanotick, duration, pitch,
                                     velocity, flags, recordUndo, noteIdOverride, sound,
                                     soundOffset);
  };

  // SET ROW OPS (81). The write half of the per-note ops the engine has been publishing since
  // v23 and v32 — retrigger, probability, the sound address, the sample offset, the onset delay.
  // Until this existed every one of them was readable and none was writable.
  //
  // ADDRESSED BY NOTE ID, not by (tick, column). The client is editing a note under a cursor and
  // knows exactly which one it means; re-deriving it from a position would reintroduce the
  // ambiguity the stable id exists to remove, and two notes can legitimately share a tick and a
  // column. `clipId` narrows the search when the caller knows it and is ignored when zero.
  //
  // Commits exactly like a note edit, because it IS one: snapshot for undo, mutate the owned
  // clip, fork it (copy-on-write, so editing a clip placed four times does not silently rewrite
  // a clip another track shares), re-derive the flat clip, bump both versions. Undo is the
  // structural whole-store snapshot rather than a fine-grained entry — restoring the notes
  // restores their ops, and a second description of a note's state is a second thing to disagree.
  auto applySetRowOps = [&](uint32_t trackId, uint32_t clipId, daw::EventId noteId,
                            const daw::RowOpEdit& edit, bool recordUndo,
                            daw::UiClipRejectReason& rejectReason) -> bool {
    return daw::engine::applySetRowOps(clipEditDeps, trackId, clipId, noteId, edit,
                                       recordUndo, rejectReason);
  };

  // Arrangement placement ops (Move/Resize/Remove/Add) all mutate a track's placement
  // store and commit exactly like a note edit: snapshot for undo, mutate, re-derive the
  // flat clip + audio render, push the undo, republish + bump the clip version so the UI
  // re-reads. `mutate` returns true if it changed anything; placements are keyed by stable
  // id. 0xFFFF... is the "leave unchanged" sentinel for Resize (a real nanotick never is).
  // M3.24: a LOCAL edit — one that belongs to THIS APPEARANCE of a clip rather than to
  // the clip itself. Recorded on the placement as an `add` (a note only this appearance
  // has) or a `mute` (a base note only this appearance is missing), which is what makes
  // "fix the bass in chorus 1, all three choruses change, and the hat you added to
  // chorus 3 survives" expressible at all: the bass fix is a CLIP edit and reaches all
  // three, the hat is a LOCAL edit and stays where it was put.
  //
  // Additive-only, on purpose (roadmap item 24): there is no "changed note" record. An
  // edit that would MODIFY a base note is decomposed into mute(original) + add(new), so
  // the override list is always a set of things added and things silenced, and reverting
  // is deleting both vectors rather than replaying inverses.
  // DOES THIS EDIT BELONG TO THE APPEARANCE OR TO THE CLIP? One function, because WriteNote and
  // DeleteNote both have to answer it and two copies would eventually disagree about the same
  // gesture — which for this feature means the same keystroke doing different things depending on
  // which handler ran.
  //
  // The explicit bit wins on its own: a caller that SAID which it meant is never overridden. The
  // placement's own flag is the standing answer for when nobody said. Never inferred from whether
  // the cell is occupied.
  // WHICH APPEARANCE IS THIS TICK IN? One lookup, for the same reason editIsLocalScope is one
  // function: the scope decision and the target decision have to agree, and they were two
  // separate loops that agreed only by accident.
  //
  // OVERLAPPING PLACEMENTS made both of them arbitrary. Each took the FIRST match in
  // sourcePlacements — file order, or insertion order, which is nothing the user can see. Worse,
  // they disagreed in a way that mattered: editIsLocalScope scanned for ANY placement under the
  // tick with localEdits set, while the target loop took the first containing placement whether
  // its flag was set or not. So with two overlapping appearances, one local and one not, the
  // gesture could be RULED local and then applied to the placement that is not — an override
  // recorded on an appearance the user never marked.
  //
  // The tie-break is the LATEST START among the placements containing the tick, and on an exact
  // tie the later one in the list. "Topmost wins" is the convention every arranger uses for
  // stacked material, and stating it is the point: an arbitrary rule that happens to be stable
  // is still unpredictable to the person using it.
  auto applyLocalNoteEdit = [&](uint32_t trackId, uint64_t nanotick, uint64_t duration,
                                uint8_t pitch, uint8_t velocity, uint8_t column,
                                bool deleting) -> bool {
    return daw::engine::applyLocalNoteEdit(clipEditDeps, trackId, nanotick, duration, pitch,
                                           velocity, column, deleting, findPlacementAt);
  };

  auto applyPlacementEdit =
      [&](uint32_t trackId,
          const std::function<bool(std::vector<daw::ProjectPlacement>&)>& mutate) -> bool {
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: placement edit — track " << trackId << " not found" << std::endl;
      return false;
    }
    std::shared_ptr<const ClipSnapshot> snapshot;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      changed = mutate(runtime->sourcePlacements);
      if (changed) {
        runtime->arrangementDirty.store(true, std::memory_order_relaxed);
        snapshot = rebuildFlatAndPublish(*runtime);
        std::atomic_store_explicit(&runtime->audioRender, rebuildAudioRender(*runtime),
                                   std::memory_order_release);
        pushStructuralUndo(trackId, std::move(before), snapshotTrackStore(*runtime));
      }
    }
    if (!changed) {
      return false;
    }
    if (snapshot) {
      std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                 std::memory_order_release);
    }
    bumpClipVersionFor(runtime);
    clipDirty.store(true, std::memory_order_release);
    // A placement edit can move the END OF THE SONG, and until this existed the loop was
    // computed once at load — so a placement added past the old end never played, and
    // nothing said why. Recomputed here, and the LOOP follows only while the user has
    // not chosen one of their own.
    recomputeSongEnd();
    return true;
  };

  auto applyAddChord = [&](uint32_t trackId, uint64_t nanotick, uint64_t duration,
                           uint8_t degree, uint8_t quality, uint8_t inversion,
                           uint8_t baseOctave, uint8_t column, uint32_t spreadNanoticks,
                           uint16_t humanizeTiming, uint16_t humanizeVelocity, bool recordUndo,
                           std::optional<uint32_t> chordIdOverride = std::nullopt) -> bool {
    return daw::engine::applyAddChord(clipEditDeps, trackId, nanotick, duration, degree,
                                      quality, inversion, baseOctave, column, spreadNanoticks,
                                      humanizeTiming, humanizeVelocity, recordUndo,
                                      chordIdOverride);
  };

  // Snapshot / restore / undo: one idea in four functions, and they moved together. See
  // apps/engine_song_store.h for why the interface is 17 members rather than the 24 captures the
  // measurement reports — HarmonyTimeline absorbs six of them and TrackTable two.
  daw::engine::SongStoreDeps songStoreDeps{
      arrange, automationVersion, clipEditDeps, clipDirty, harmonyTimeline, songTiming,
      tempoProvider, trackTable, buildTrackSnapshot, bumpClipVersionFor, ensurePlacementIds,
      rebuildAudioRender, rebuildFlatAndPublish, recomputeSongEnd, snapshotTracks,
      emitClipResync};
  auto snapshotSongStore = [&]() { return daw::engine::snapshotSongStore(songStoreDeps); };
  auto restoreSongStore = [&](const SongStoreState& state) {
    return daw::engine::restoreSongStore(songStoreDeps, state);
  };
  auto restoreTrackStore = [&](uint32_t trackId, const TrackStoreState& state) {
    return daw::engine::restoreTrackStore(songStoreDeps, trackId, state);
  };
  auto applyUndoEntry = [&](const daw::UndoEntry& entry, bool recordUndo) {
    return daw::engine::applyUndoEntry(songStoreDeps, entry, recordUndo);
  };

  // The absolute anchor of the first placement referencing an owned clip id
  // (0 if none) — used to shift a clip-relative remove result onto the timeline.
  // ---- THE INWARD BULK CARRIER (opcode 83).
  //
  // Reassembly state for messages too long for one 40-byte ring payload. Lives here, in the UI
  // command thread's scope, because that thread is the only one that drains the ring — the same
  // reason every other handler below keeps its state here rather than behind a lock.
  std::vector<BulkStream> bulkStreams;
  uint64_t bulkTick = 0;

  // Dispatch an ASSEMBLED bulk payload. Its first uint16 is the real commandType, so a bulk
  // command looks exactly like a small one at this point and there is one dispatch rule rather
  // than two — the carrier is a transport detail and nothing downstream needs to know a message
  // arrived in pieces.
  daw::engine::AssembledBulkDeps assembledBulkDeps{
      bumpClipVersionFor, clipDirty, publishAudioClipTable, rebuildAudioRender,
      rebuildFlatAndPublish, refreshSamplerForTrack, reportSamplerReject,
      requireMatchingClipVersion, resolveSourcePath, trackTable};

  auto handleAssembledBulk = [&](const std::vector<uint8_t>& buf) {
    daw::engine::handleAssembledBulk(assembledBulkDeps, buf);
  };

  // WHAT THE SAMPLER COMMANDS NEED, named once instead of implied by a [&] capture.
  //
  // The eleven sampler dispatch blocks moved to apps/engine_sampler_commands.cpp. They were the
  // largest family in the dispatcher (1,411 lines) and the least entangled — seven names against
  // the transport family's thirty-five — which is why they went first. The dispatcher itself has
  // since followed them out of main(), to apps/engine_handle_ui_entry.cpp.
  //
  // These std::function objects wrap lambdas that are still defined above and still capture by
  // reference; the struct holds references to THESE, so all of it lives exactly as long as main's
  // scope. Command-thread only, so the indirection costs nothing that matters here.
  const std::function<void(daw::UiCommandType, daw::UiSamplerRejectReason,
                           uint32_t, uint32_t, uint16_t)> reportSamplerRejectFn =
      reportSamplerReject;
  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint16_t, bool,
                           std::optional<daw::EventId>, uint16_t, uint16_t)> applyAddNoteFn =
      [&](uint32_t t, uint64_t n, uint64_t d, uint8_t p, uint8_t v, uint16_t f, bool u,
          std::optional<daw::EventId> id, uint16_t snd, uint16_t so) {
        return applyAddNote(t, n, d, p, v, f, u, id, snd, so);
      };
  daw::engine::SamplerCommandDeps samplerCommandDeps{
      uiShm, trackTable, tempoProvider, samplerRefreshDeps,
      reportSamplerRejectFn, rebuildSamplerRenderFn, applyAddNoteFn};

  // The automation and clip-field commands moved out too; same shape as the sampler family.
  const std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)>
      buildTrackSnapshotFn = buildTrackSnapshot;
  const std::function<bool(const TrackRuntime&)> trackIsPersistedFn = trackIsPersisted;
  const std::function<bool(uint32_t, daw::UiCommandType, uint32_t)>
      requireMatchingClipVersionFn = requireMatchingClipVersion;
  daw::engine::AutomationCommandDeps automationCommandDeps{
      trackTable, automationVersion, uiShm,
      buildTrackSnapshotFn, historyAppendFn, trackIsPersistedFn,
      requireMatchingClipVersionFn};

  const std::function<uint32_t(TrackRuntime*)> bumpClipVersionForFn = bumpClipVersionFor;
  const std::function<void()> publishAudioClipTableFn = publishAudioClipTable;
  const std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)>
      rebuildAudioRenderFn = rebuildAudioRender;
  const std::function<void(bool)> writeUiClipExtentsFn = writeUiClipExtents;
  daw::engine::ClipCommandDeps clipCommandDeps{
      trackTable, clipVersion, uiShm,
      bumpClipVersionForFn, publishAudioClipTableFn, rebuildAudioRenderFn, writeUiClipExtentsFn};

  const std::function<void(uint16_t, uint32_t, uint32_t)> emitModErrorFn = emitModError;
  const std::function<void(TrackRuntime&)> emitModSnapshotFn = emitModSnapshot;
  daw::engine::ModlinkCommandDeps modlinkCommandDeps{
      trackTable, buildTrackSnapshotFn, emitModErrorFn, emitModSnapshotFn,
      historyAppendFn};

  const std::function<void(uint32_t, uint16_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t)> emitPatcherGraphDeltaFn = emitPatcherGraphDelta;
  const std::function<void(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t)> emitPatcherGraphErrorFn = emitPatcherGraphError;
  const std::function<void(const daw::UiDiffPayload&)> emitUiDiffFn = emitUiDiff;
  // snapshotTracksFn MOVED UP TO HERE from further down, rather than a second wrapper being made
  // beside it. PatcherCommandDeps below takes the reassembly as a std::function, so its deps must
  // exist by then; snapshotTracks itself has existed since line ~1466 and only its wrapper was
  // late. A second wrapper would also have needed a second NAME, and deps_order_check reads the
  // name — it rejected snapshotTracksPaFn against member snapshotTracks, correctly.
  daw::engine::PatcherAssembleDeps patcherAssembleDeps{patcherGraph, trackTable, snapshotTracksFn};
  auto reassemblePatcherFromDevices = [&] {
    return daw::engine::reassemblePatcherFromDevices(patcherAssembleDeps);
  };
  const std::function<bool()> reassemblePatcherFromDevicesFn = reassemblePatcherFromDevices;
  const std::function<void()> updatePatcherGraphSnapshotFn = updatePatcherGraphSnapshot;
  daw::engine::PatcherCommandDeps patcherCommandDeps{
      trackTable, patcherGraph, buildTrackSnapshotFn, emitPatcherGraphDeltaFn,
      emitPatcherGraphErrorFn, emitUiDiffFn, reassemblePatcherFromDevicesFn,
      updatePatcherGraphSnapshotFn
  };

  const std::function<bool(const std::string&, std::string*)> saveProjectToPathFn =
      saveProjectToPath;
  const std::function<bool(const std::string&, std::string*)> loadProjectFromPathFn =
      loadProjectFromPath;
  daw::engine::ModuleCommandDeps moduleCommandDeps{
      loadedProjectDir, saveProjectToPathFn, loadProjectFromPathFn};

  const std::function<void(uint16_t, uint32_t)> emitRoutingErrorFn = emitRoutingError;
  const std::function<void(TrackRuntime&)> emitRoutingSnapshotFn = emitRoutingSnapshot;

  daw::engine::MarkerCommandDeps markerCommandDeps{
      arrange, historyAppendFn
  };

  daw::engine::ProjectCommandDeps projectCommandDeps{
      projectLoadOk, projectLoadSeq, saveProjectToPathFn, loadProjectFromPathFn};

  const std::function<void(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t)> emitChainErrorFn =
      emitChainError;
  const std::function<void(TrackRuntime&)> emitChainSnapshotFn = emitChainSnapshot;
  const std::function<void()> reconcileMasterHostFn = reconcileMasterHost;
  const std::function<void(TrackRuntime&)> refreshSamplerForTrackFn2 = refreshSamplerForTrack;
  daw::engine::ChainCommandDeps chainCommandDeps{
      trackTable, masterTrack, transport, pluginCache, buildTrackSnapshotFn,
      emitChainErrorFn, emitChainSnapshotFn, rebuildHostForChainFn, reconcileMasterHostFn,
      refreshSamplerForTrackFn2
  };

  const std::function<bool(uint32_t, uint32_t, daw::EventId, const daw::RowOpEdit&, bool,
                           daw::UiClipRejectReason&)> applySetRowOpsFn = applySetRowOps;
  daw::engine::RowopsCommandDeps rowopsCommandDeps{applySetRowOpsFn, emitClipRejectFn};

  const std::function<std::string(const std::string&)> resolveSourcePathFn = resolveSourcePath;
  const std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)>
      resolveDevicePluginPathFn = resolveDevicePluginPath;
  daw::engine::RequestCommandDeps requestCommandDeps{
      uiShm, trackTable, waveformStore, clipWindowMutex, clipWindowPending,
      resolveSourcePathFn, resolveDevicePluginPathFn, rebuildHostForChainFn,
      emitChainSnapshotFn};

  const std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)>
      rebuildFlatAndPublishFn = rebuildFlatAndPublish;
  daw::engine::ArrangeTimeCommandDeps arrangeTimeCommandDeps{
      arrange, automationVersion, buildTrackSnapshot, bumpClipVersionFor, clipDirty,
      harmonyTimeline, historyAppend, songTiming, pushUndo, rebuildAudioRender,
      rebuildFlatAndPublish, recomputeSongEnd, snapshotSongStore, snapshotTracks,
      tempoProvider
  };
  daw::engine::TrackpropsCommandDeps trackpropsCommandDeps{
      trackTable, masterTrack, quantizeVersion,
      buildTrackSnapshotFn, rebuildFlatAndPublishFn};

  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint8_t, bool)>
      applyLocalNoteEditFn = applyLocalNoteEdit;
  const std::function<bool(uint32_t, uint64_t, uint16_t)> editIsLocalScopeFn = editIsLocalScope;
  const std::function<bool(uint32_t, uint64_t, uint8_t, uint16_t, bool)> applyRemoveNoteFn =
      [&](uint32_t trackId, uint64_t nanotick, uint8_t pitch, uint16_t flags, bool recordUndo) {
        return daw::engine::applyRemoveNote(clipEditDeps, trackId, nanotick, pitch, flags,
                                            recordUndo);
      };
  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint8_t, uint8_t,
                           uint8_t, uint32_t, uint16_t, uint16_t, bool, std::optional<uint32_t>)>
      applyAddChordFn = applyAddChord;
  const std::function<bool(uint32_t, uint32_t, bool)> applyRemoveChordFn =
      [&](uint32_t trackId, uint32_t chordId, bool recordUndo) {
        return daw::engine::applyRemoveChord(clipEditDeps, trackId, chordId, recordUndo);
      };
  const std::function<bool(uint32_t, uint64_t, uint8_t, bool)> applyRemoveChordAtFn =
      [&](uint32_t trackId, uint64_t nanotick, uint8_t column, bool recordUndo) {
        return daw::engine::applyRemoveChordAt(clipEditDeps, trackId, nanotick, column,
                                               recordUndo);
      };
  const std::function<bool(uint64_t, uint32_t, uint32_t, bool)> addOrUpdateHarmonyFn =
      addOrUpdateHarmony;
  const std::function<bool(uint64_t, bool)> removeHarmonyFn = removeHarmony;
  const std::function<bool(uint32_t, daw::UiCommandType)> requireMatchingHarmonyVersionFn =
      requireMatchingHarmonyVersion;
  daw::engine::NoteCommandDeps noteCommandDeps{
      applyAddNoteFn, applyLocalNoteEditFn, editIsLocalScopeFn, applyRemoveNoteFn,
      applyAddChordFn, applyRemoveChordFn, applyRemoveChordAtFn, addOrUpdateHarmonyFn,
      removeHarmonyFn, requireMatchingClipVersionFn, requireMatchingHarmonyVersionFn};

  const std::function<bool(const daw::UndoEntry&, bool)> applyUndoEntryFn = applyUndoEntry;
  const std::function<bool(const SongStoreState&)> restoreSongStoreFn = restoreSongStore;
  const std::function<bool(uint32_t, const TrackStoreState&)> restoreTrackStoreFn =
      restoreTrackStore;
  daw::engine::UndoCommandDeps undoCommandDeps{
      trackTable, undoMutex, undoStack, redoStack,
      applyUndoEntryFn, restoreSongStoreFn, restoreTrackStoreFn, requireMatchingClipVersionFn};

  const std::function<TrackRuntime*(uint32_t, const std::string&)> ensureTrackFn = ensureTrack;
  const std::function<std::optional<std::string>(uint32_t)> resolvePluginPathFn =
      resolvePluginPath;
  const std::function<void(TrackRuntime&, uint32_t)> updateTrackChainForInstrumentFn =
      updateTrackChainForInstrument;
  daw::engine::DeviceCommandDeps deviceCommandDeps{
      trackTable, transport, audioPlaybackBlockId, pluginPath,
      resolveDevicePluginPathFn, rebuildHostForChainFn, emitChainSnapshotFn, ensureTrackFn,
      resolvePluginPathFn, updateTrackChainForInstrumentFn
  };

  // std::function wrappers so the Deps struct can hold references with a lifetime. A raw
  // lambda bound to a const std::function& would create a temporary that dies at the end of
  // the full expression, leaving the struct holding a dangling reference.
  const std::function<bool(uint32_t, const std::function<bool(std::vector<daw::ProjectPlacement>&)>&)> applyPlacementEditFn = applyPlacementEdit;
  const std::function<void(uint32_t, uint8_t, uint8_t, bool)> enqueuePreviewFn = enqueuePreview;
  const std::function<void(const std::vector<uint8_t>&)> handleAssembledBulkFn = handleAssembledBulk;
  const std::function<void(uint32_t, TrackStoreState, TrackStoreState)> pushStructuralUndoFn = pushStructuralUndo;
  const std::function<void(EngineUndoEntry)> pushUndoFn = pushUndo;
  const std::function<void()> recomputeSongEndFn = recomputeSongEnd;
  const std::function<void(TrackRuntime&)> resetTrackContentFn = resetTrackContent;
  const std::function<bool(TrackRuntime&, const std::vector<std::string>&)> restartTrackHostFn = restartTrackHost;
  const std::function<std::unique_ptr<TrackRuntime>(uint32_t, const std::string&, bool, bool)> setupTrackRuntimeFn = setupTrackRuntime;
  const std::function<SongStoreState()> snapshotSongStoreFn = snapshotSongStore;
  const std::function<TrackStoreState(const TrackRuntime&)> snapshotTrackStoreFn = snapshotTrackStore;

  daw::engine::TrackCommandDeps trackCommandDeps{
      buildTrackSnapshotFn, bumpClipVersionForFn, clipVersion, emitRoutingErrorFn,
      emitRoutingSnapshotFn, liveTrackCount, rebuildAudioRenderFn, rebuildFlatAndPublishFn,
      resetTrackContentFn, restartTrackHostFn, setupTrackRuntimeFn, trackTable};
  daw::engine::PlacementCommandDeps placementCommandDeps{
      applyPlacementEditFn, bumpClipVersionForFn, clipDirty, historyAppendFn, nextClipId,
      nextPlacementId, pushStructuralUndoFn, pushUndoFn, rebuildAudioRenderFn,
      rebuildFlatAndPublishFn, recomputeSongEndFn, requireMatchingClipVersionFn,
      snapshotTrackStoreFn, trackTable};

  daw::engine::TransportCommandDeps transportCommandDeps{
      heldPreview, songTiming, transport, masterTrack, panicPending, patternTicks,
      pendingPreviewNotes, previewMutex, resetTimeline, restartCv, running, tempoProvider,
      trackTable
  };

  daw::engine::HandleUiEntryDeps handleUiEntryDeps{
      arrangeTimeCommandDeps, automationCommandDeps, bulkStreams, bulkTick,
      chainCommandDeps, clipCommandDeps, deviceCommandDeps, enqueuePreviewFn,
      handleAssembledBulkFn, historyAppendFn, markerCommandDeps, modlinkCommandDeps,
      moduleCommandDeps, noteCommandDeps, patcherCommandDeps, placementCommandDeps,
      projectCommandDeps, requestCommandDeps, rowopsCommandDeps, samplerCommandDeps,
      trackCommandDeps, trackpropsCommandDeps, transportCommandDeps, undoCommandDeps
  };

  auto handleUiEntry = [&](const daw::EventEntry& entry) {
    daw::engine::handleUiEntry(handleUiEntryDeps, entry);
  };

  // The three ring accessors and uiDiffNowMs are lambdas; UiThreadDeps holds std::function
  // REFERENCES, so each needs a named object to bind to. A temporary would dangle the moment this
  // statement ended, and the thread reads them for the life of the process.
  const std::function<daw::EventRingView()> getRingUiFn = getRingUi;
  const std::function<daw::EventRingView()> getRingUiAgentFn = getRingUiAgent;
  const std::function<daw::UiEditRingView()> getRingUiEditFn = getRingUiEdit;
  const std::function<uint64_t()> uiDiffNowMsFn = uiDiffNowMs;
  const std::function<void(const daw::EventEntry&)> handleUiEntryFn = handleUiEntry;
  daw::engine::UiThreadDeps uiThreadDeps{
      running, getRingUiFn, getRingUiAgentFn, getRingUiEditFn, handleUiEntryFn, uiDiffNowMsFn};
  std::thread uiThread([&] { daw::engine::runUiThread(uiThreadDeps); });
  daw::LogLine() << "UI: command thread launched" << std::endl;

  daw::engine::ProducerThreadDeps producerThreadDeps{
     audioPlaybackBlockId, engineConfig, enqueuePreview, getHarmonyAt, getRingCtrl, getRingStd,
      getScaleForHarmony, harmonyTimeline, lastOverflowTick, latencyMgr, nextBlockId,
      nextNoteId, offlineProducerArmed, offlineRender, panicPending, patcherGraph,
      patcherParallel, patcherPool, patternTicks, pendingPreviewNotes, poolAlwaysOn,
      poolEngaged, poolWorkEwmaUs, previewMutex, producerBlocksOverBudget, producerBlocksTimed,
      producerBlockUsMax, producerBlockUsTotal, producerSamplerUsMax, producerSamplerUsTotal,
      projectSeed, publishedCallback, quantizePitch, renderPool, resetTimeline,
      resolveDevicePluginPath, running, snapshotTracks, songTiming, tempoProvider,
      testThrottleMs, tickConverter, traceNotes, transport, warnedEventOutsideBlock,
      writeMirrorParams};
  std::thread producer([&] { daw::engine::runProducerThread(producerThreadDeps); });

  daw::engine::UiWriterDeps uiWriterDeps{
      arrangeGeneration, arrange, automationGeneration, automationVersion, clipVersion,
      clipWindowMutex, clipWindowPending, harmonyTimeline, laneQuantizeOf,
      lastArrangeSongEnd, lastArrangeVersion, lastAutomationVersion,
      lastClipAllQuantizeVersion, lastClipAllVersion, lastPatcherVersion, patcherGraph,
      quantizeVersion, snapshotTracks, songTiming, trackIsPersisted, uiShm,
      warnedPatcherOwnerTooWide
  };

  daw::engine::ConsumerDeps consumerDeps{
      audioPlaybackBlockId, auxChildOverlayMutex, auxChildOverlays, buildTrackSnapshot,
      clipVersion, engineConfig, ensurePlacementIds, harmonyTimeline, lastOverflowTick,
      latencyMgr, liveTrackCount, loadInProgress, transport, masterTrack, maxUiTracks,
      pdcDisabled, projectLoadOk, projectLoadSeq, publishedCallback, quantizeVersion,
      rebuildAudioRender, rebuildFlatAndPublish, reconcileChildTracks, running,
      samplerKitVersion, scheduleHostRestart, snapshotTracks, songTiming, tempoProvider,
      uiShm, uiWriterDeps, writeUiClipExtents
  };

  std::thread consumer([&] { daw::engine::runConsumerThread(consumerDeps); });
  // The audio parameters the mix is built at. With a device they are the DEVICE's (adopted
  // earlier, because a hardcoded 48 kHz plays everything off-speed on any other rate). Offline
  // there is no device, so the engine's own config stands — the same numbers the producer, the
  // per-track SHM stride and the hosts were already configured with, so every stage still
  // agrees on samples-per-block.
  // --sample-rate WINS over the device, for the same reason --block-size does below, and this
  // line had the same two-sources-of-truth defect that comment describes: the rate was read
  // STRAIGHT off the backend here while baseConfig carried its own, so an override applied to the
  // config never reached the render pump. Block size had already been fixed; the rate next to it
  // had not, which is what a duplicated rule looks like after one of its copies is repaired.
  const double effSampleRate =
      forcedSampleRate > 0.0
          ? forcedSampleRate
          : (audioBackend ? audioBackend->sampleRate() : engineConfig.sampleRate);
  // --block-size WINS over the device's buffer. Without this the engine's config and the render
  // pump disagreed: the config took the forced size while the pump kept taking the device's, so
  // the callback strode 512 frames through 64-frame buffers and produced audio that was garbage
  // in a plausible-sounding way. Caught by the determinism check on its first end-to-end run —
  // which is the check doing exactly its job, on the tooling rather than on the sampler.
  const uint32_t effBlockSize =
      forcedBlockSize > 0
          ? forcedBlockSize
          : (audioBackend ? static_cast<uint32_t>(audioBackend->blockSize())
                          : engineConfig.blockSize);
  const int effOutChannels = audioBackend ? audioBackend->outputChannels() : 2;
  if (!testMode) {
    daw::engine::AudioStartDeps audioStartDeps{
        audioBackend, audioCallback, audioCallbackPublished, audioPlaybackBlockId,
        audioRuntime, effBlockSize, effOutChannels, effSampleRate, engineConfig,
        masterFxActive, masterRenderDeps, masterRenderThread, masterTrack, offlineChannels,
        offlineRender,
        transport};
    daw::engine::startAudioDevice(audioStartDeps);
  }

  // Underrun reporter: a low-priority watcher that stays silent while the audio thread
  // meets every block deadline and speaks up the moment it starts dropping blocks, so
  // glitching is reported as a concrete count rather than a vague feeling. It never
  // touches the audio thread beyond reading relaxed atomics.
  std::thread xrunReporter;
  // Declared out here, not inside the `if`: see XrunReporterDeps for why the scope matters.
  daw::engine::XrunReporterDeps xrunReporterDeps{
      running, audioCallback, transport, nextBlockId, audioPlaybackBlockId,
      observedPipelineBlocks
  };
  if (!testMode && audioCallback) {
    const double blockMs = engineConfig.sampleRate > 0.0
        ? static_cast<double>(engineConfig.blockSize) /
              engineConfig.sampleRate * 1000.0
        : 0.0;
    const bool latencyReport = std::getenv("DAW_ENGINE_LATENCY_REPORT") != nullptr;
    xrunReporter = std::thread([&, blockMs, latencyReport] {
      daw::engine::runXrunReporter(xrunReporterDeps, blockMs, latencyReport);
    });
  }

  // --project: load before anything runs. For a render this is mandatory (the pump starts as
  // soon as the threads are up, so there is no window for a CLI load); on its own it just saves
  // a round trip. Reported loudly on failure and the render is abandoned rather than writing a
  // file of silence, which is what the first version did and it looked exactly like success.
  bool startupLoadFailed = false;
  if (!startupProject.empty()) {
    const std::filesystem::path path = std::filesystem::path(daw::defaultProjectDir()) /
                                       (startupProject + ".uniproj.json");
    std::string error;
    const bool ok = loadProjectFromPath(path.string(), &error);
    projectLoadOk.store(ok ? 1u : 0u, std::memory_order_release);
    projectLoadSeq.fetch_add(1, std::memory_order_acq_rel);
    DAW_EVENT("project.load")
        .field("path", path.string())
        .field("ok", ok)
        .field("startup", true)
        .field("error", ok ? std::string() : error);
    if (!ok) {
      daw::LogLine() << "Startup load FAILED for " << path.string() << ": " << error << std::endl;
      startupLoadFailed = true;
    } else {
      std::cout << "Startup load: " << path.string() << std::endl;
      // No sleep here: a render waits for a host to be READY (awaitAnyReadyTrack), which is
      // the condition that actually matters, and a fixed guess would be both slower and
      // occasionally wrong.
    }
  }
  if (offlineRender && startupLoadFailed) {
    daw::LogLine() << "Offline render abandoned: nothing was loaded to render" << std::endl;
    renderFailed = true;
    running.store(false);
  } else if (offlineRender && audioCallback) {
    daw::engine::OfflineRenderDeps offlineRenderDeps{
        audioCallback.get(), effBlockSize, effSampleRate, offlineChannels,
        offlineProducerArmed, renderFailed, renderName, resetTimeline, runSeconds,
        running, songTiming, tempoProvider, transport};
    daw::engine::runOfflinePump(offlineRenderDeps);
  } else if (runSeconds >= 0) {
    std::this_thread::sleep_for(std::chrono::seconds(runSeconds));
    running.store(false);
  }
  restartCv.notify_all();
  daw::engine::ShutdownDeps shutdownDeps{
      audioBackend, audioCallback, consumer, engineConfig, masterFxActive,
      masterRenderThread, observedPipelineBlocks, producer, producerBlockUsMax,
      producerBlockUsTotal, producerBlocksOverBudget, producerBlocksTimed,
      producerSamplerUsMax, producerSamplerUsTotal, restartWorker, trackTable,
      uiThread, uiShm, xrunReporter};
  daw::engine::shutdownEngine(shutdownDeps);

  // A render that stalled or had nothing to render exits NON-ZERO. A shell check that reads
  // only the exit code must not be told a silent or truncated file was a success — the whole
  // point of the loud-failure discipline is that the caller does not have to go looking.
  if (renderFailed) {
    return 2;
  }
  return 0;
}
