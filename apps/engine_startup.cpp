#include "engine_startup.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "engine_types.h"
#include "event_log.h"
#include "shared_memory.h"

namespace daw::engine {

int parseEngineArgs(int argc, char** argv, EngineArgs& out) {
  // Named bindings into `out`, so the loop below is the one main() carried, unchanged.
  auto& socketPath = out.socketPath;
  auto& pluginPath = out.pluginPath;
  auto& spawnHost = out.spawnHost;
  auto& runSeconds = out.runSeconds;
  auto& renderName = out.renderName;
  auto& forcedBlockSize = out.forcedBlockSize;
  auto& forcedSampleRate = out.forcedSampleRate;
  auto& startupProject = out.startupProject;
  auto& noAudio = out.noAudio;

  // `i < argc`, not `i + 1 < argc`: the old bound meant a flag with NO value was
  // invisible when it came last, so `daw_engine --no-spawn` silently spawned.
  // Flags that take a value check for one themselves.
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool hasValue = (i + 1) < argc;
    if (arg == "--socket" && hasValue) {
      socketPath = argv[i + 1];
      ++i;
    } else if (arg == "--plugin" && hasValue) {
      pluginPath = std::filesystem::absolute(argv[i + 1]).string();
      ++i;
    } else if (arg == "--no-spawn") {
      spawnHost = false;
    } else if (arg == "--no-audio") {
      // Run the whole engine with no audio DEVICE: the transport still advances,
      // the UI still publishes, plugins still load — there is simply no output.
      // Added because every measurement of the transport used to require putting
      // sound through somebody's speakers, which makes a test suite something you
      // cannot run while a person is in the room.
      noAudio = true;
    } else if (arg == "--run-seconds" && hasValue) {
      runSeconds = std::max(0, std::atoi(argv[i + 1]));
      ++i;
    } else if (arg == "--project" && i + 1 < argc) {
      // Load this project at startup. Required for --render, because the pump begins as soon
      // as the threads are up — there is no window in which a CLI could send a load, and the
      // first render I ran produced a perfectly-sized file of pure silence for exactly that
      // reason.
      startupProject = argv[i + 1];
      ++i;
    } else if (arg == "--render" && i + 1 < argc) {
      // OFFLINE RENDER (§7 Q4). Runs the whole mix with no audio device and no wall clock:
      // the pump waits for every host to finish each block, then mixes it, so the render is
      // glitch-free by construction rather than by luck. The producer already paces to the
      // block the CONSUMER has played rather than to a device clock — a consequence of
      // fixing the "everything 4x too fast" bug — so being the consumer is all that is
      // needed to run at host speed.
      renderName = argv[i + 1];
      ++i;
    } else if (arg == "--block-size" && i + 1 < argc) {
      // Forces the engine's block size, which the offline render otherwise takes from its
      // default (there is no audio device to ask). It exists so BLOCK-SIZE INVARIANCE is
      // checkable end to end and not only in a unit test: docs/SAMPLER_DESIGN.md §3.5 requires
      // one project rendered at 64, 256 and 1024 frames to be bit-identical, and a property
      // that cannot be exercised through the real engine is a property nobody is defending.
      forcedBlockSize = static_cast<uint32_t>(std::max(1, std::atoi(argv[i + 1])));
      ++i;
    } else if (arg == "--sample-rate" && i + 1 < argc) {
      // RENDER AT A STATED RATE INSTEAD OF WHATEVER IS PLUGGED IN.
      //
      // Without this the offline render adopts the DEFAULT OUTPUT DEVICE's rate, so what a bounce
      // contains depends on the machine's audio settings at that moment. That is wrong twice
      // over. As a product: delivering at 48k while your interface sits at 44.1k is an ordinary
      // requirement, and the only way to ask for it was to go and change the system's default
      // output device. As a test instrument: the byte-deterministic render is what the whole
      // engine refactor is gated on, and it silently stopped being deterministic whenever
      // somebody connected headphones — the default went to 48000 and back to 44100 within an
      // hour, with nothing in the log to say so, and a check that had passed for weeks failed
      // the engine for being correct.
      //
      // REFUSED RATHER THAN CLAMPED if it is outside what an audio path can mean. A silent
      // fallback to the device rate is precisely how the original problem stayed invisible:
      // the render would claim to honour a rate it had ignored.
      const double asked = std::atof(argv[i + 1]);
      if (asked < 8000.0 || asked > 384000.0) {
        std::cerr << "--sample-rate " << argv[i + 1]
                  << " is outside 8000..384000 Hz; refusing rather than falling back to the "
                     "device rate, which would render at a rate you did not ask for"
                  << std::endl;
        return 2;
      }
      forcedSampleRate = asked;
      ++i;
    }
  }
  return 0;
}

void readStartupEnvironment(EngineArgs& out) {
  auto& testMode = out.testMode;
  auto& testThrottleMs = out.testThrottleMs;
  auto& patcherParallel = out.patcherParallel;
  auto& pdcDisabled = out.pdcDisabled;
  auto& traceNotes = out.traceNotes;

  if (const char* env = std::getenv("DAW_ENGINE_TEST_MODE")) {
    testMode = std::string(env) == "1";
  }
  testThrottleMs = 0;
  if (const char* env = std::getenv("DAW_ENGINE_TEST_THROTTLE_MS")) {
    char* end = nullptr;
    const long value = std::strtol(env, &end, 10);
    if (end != env && value > 0) {
      testThrottleMs = static_cast<int>(value);
    }
  }
  patcherParallel = false;
  if (const char* env = std::getenv("DAW_PATCHER_PARALLEL")) {
    patcherParallel = std::string(env) == "1";
  }
  // Movement 4 PDC kill-switch. Off = compensation active (the default). Set to "1"
  // to force zero compensation across all tracks — an A/B escape hatch (some engineers
  // want plugin latency left uncompensated for tracking) and the lever the PDC audio
  // test toggles to show alignment appears only when compensation runs.
  pdcDisabled = [] {
    const char* env = std::getenv("DAW_DISABLE_PDC");
    return env != nullptr && std::string(env) == "1";
  }();
  // Trace every scheduled note-on (tick + pitch) to the event log. Off by
  // default; a verification aid — counts and times the notes the scheduler
  // actually emits, independent of any synth's audio. Runs on the producer
  // thread (same one that already locks and does I/O), never the audio callback.
  traceNotes = std::getenv("DAW_TRACE_NOTES") != nullptr;
}

EngineDevice openAudioDevice(EngineArgs& args) {
  // Named bindings so the sequence below is the one main() carried, unchanged.
  auto& testMode = args.testMode;
  auto& pluginPath = args.pluginPath;
  auto& socketPath = args.socketPath;
  auto& noAudio = args.noAudio;
  auto& forcedBlockSize = args.forcedBlockSize;
  auto& forcedSampleRate = args.forcedSampleRate;
  EngineDevice out;
  auto& baseConfig = out.baseConfig;
  auto& audioRuntime = out.audioRuntime;
  auto& audioBackend = out.audioBackend;

  if (testMode) {
    pluginPath.clear();
  } else if (pluginPath.empty()) {
    // JUCE writes plugin artefacts to <target>_artefacts/<CONFIG>/VST3. Only
    // the unsuffixed layout was probed here, which no build produces any more —
    // so this found the plugin solely in build directories old enough to still
    // hold a leftover identity_plugin_artefacts/VST3 from a much earlier build,
    // and found nothing in a freshly created one. That is why two checkouts of
    // the same source behaved differently: one engine came up with Identity
    // loaded, the other silently came up with no plugin at all. Probe both.
    const std::filesystem::path roots[] = {"identity_plugin_artefacts",
                                           "build/identity_plugin_artefacts",
                                           "../build/identity_plugin_artefacts"};
    const std::string configs[] = {"", "RelWithDebInfo", "Release", "Debug",
                                   "MinSizeRel"};
    for (const auto& root : roots) {
      for (const auto& config : configs) {
        std::filesystem::path candidate = config.empty() ? root : root / config;
        candidate /= "VST3/Identity.vst3";
        if (std::filesystem::exists(candidate)) {
          pluginPath = std::filesystem::absolute(candidate).string();
          std::cout << "No plugin specified; using " << pluginPath << std::endl;
          break;
        }
      }
      if (!pluginPath.empty()) {
        break;
      }
    }
  }

  baseConfig.socketPath = socketPath;
  if (!pluginPath.empty()) {
    baseConfig.pluginPaths = {pluginPath};
    baseConfig.pluginNames = {""};  // name-agnostic; rebuildHostForChain fills it
  }
  baseConfig.sampleRate = 48000.0;  // fallback only; overridden by the device
  // The per-track input plane carries the main input in channels [0, numChannelsOut)
  // and a stereo sidechain (key) input in the channels after it (Movement 4). Widening
  // it unconditionally keeps the SHM layout uniform; a track with no sidechain route
  // just leaves those channels silent, and a plugin without a sidechain bus ignores
  // them. This is what lets the engine key a compressor off another track's output.
  // ...AND an aux INPUT plane of the same width as the aux output plane, so an IN-ENGINE
  // instrument's stems can reach the child tracks.
  //
  // The aux OUTPUT plane exists for a multi-out PLUGIN: the plugin writes its stems there and
  // reconcileChildTracks derives a child per bus. The built-in sampler is not a plugin — it
  // renders in the engine — so it had no way to reach that plane at all, and S6 in
  // SAMPLER_DESIGN assumed otherwise. This is the fix: the sampler writes its stems into the
  // LAST numAuxChannelsOut channels of the INPUT plane, and the host copies aux-in to aux-out
  // before its plugins run. The sampler's audio then travels the same route as everything else
  // — through the chain — rather than needing a private path around it.
  //
  // The offset is DERIVED on both sides as (numChannelsIn - numAuxChannelsOut) rather than sent
  // as a third field, so the two cannot disagree about where the plane starts.
  baseConfig.numChannelsIn =
      baseConfig.numChannelsOut + kSidechainChannels + kMaxAuxOutputChannels;
  // Movement 4 multi-out: reserve the aux OUTPUT plane so a multi-out instrument's stems
  // reach the engine for its child tracks. Sized once here for every host; a track
  // without a multi-out plugin just never writes it.
  baseConfig.numAuxChannelsOut = kMaxAuxOutputChannels;
  // Pipeline depth: how many blocks the producer may run ahead of the audio device.
  // It is the entire headroom for absorbing jitter in async out-of-process host
  // rendering AND the dominant transport-to-ear latency (each block is
  // blockSize/sampleRate seconds), so it is the direct knob for the glitch<->latency
  // trade. Default 3 (~23 ms transport-to-ear at 512/44.1k, + the device buffer): with
  // the render thread realtime-scheduled a 2-block-deep pipeline holds without starving,
  // measured. A heavier real-plugin session that the underrun reporter flags can raise it
  // via DAW_ENGINE_NUM_BLOCKS. Clamped to [2, 32] — below 2 the ring can't double-buffer.
  baseConfig.numBlocks = 3;
  if (const char* nbEnv = std::getenv("DAW_ENGINE_NUM_BLOCKS")) {
    const int want = std::atoi(nbEnv);
    if (want >= 2) {
      baseConfig.numBlocks = static_cast<uint32_t>(std::min(want, 32));
    }
  }
  baseConfig.ringUiCapacity = 1024;

  // Adopt the audio device's ACTUAL sample rate before anything (hosts, the
  // SHM header, the scheduler threads) captures the config. Hardcoding 48 kHz
  // plays everything off-speed on any other device — 48k content on a 96k
  // device runs 2x fast, on 192k 4x fast. Opened here to read the rate; started
  // later. If there is no device, the 48 kHz fallback stands for offline timing.
  // JUCE FIRST, DEVICE SECOND. `ScopedJuceInitialiser_GUI` (inside the runtime) brings up the
  // MessageManager, and this used to be constructed seventeen thousand lines further down —
  // AFTER the CoreAudio device was opened to read its sample rate. On this machine the device
  // then opened, reported its name, rate and block size, answered isPlaying() with true, and
  // never ran a single IO callback: the app made no sound at all, every capture came back empty,
  // and both agents wrote it up as a dead audio device.
  if (!noAudio) {
    audioRuntime = daw::createJuceRuntime();
  }
  audioBackend = noAudio ? nullptr : daw::createAudioBackend();
  if (noAudio) {
    std::cout << "--no-audio: no output device; " << baseConfig.sampleRate
              << " Hz assumed for timing" << std::endl;
  }
  if (audioBackend && audioBackend->openDefaultDevice(2)) {
    baseConfig.sampleRate = audioBackend->sampleRate();
    // Adopt the device's ACTUAL buffer size too (not just its sample rate). The whole
    // pipeline — per-track SHM block stride, the producer, and the audio callback — must
    // agree on samples-per-block; the callback is built from the device size, so if the
    // device's buffer is anything but the 512 default (a smaller/larger native size, or
    // a DAW_ENGINE_BUFFER_SIZE override) the host would render mis-sized blocks and the
    // callback would read past them. Adopting it here keeps every stage consistent.
    if (audioBackend->blockSize() > 0) {
      baseConfig.blockSize = static_cast<uint32_t>(audioBackend->blockSize());
    }
    std::cout << "Audio device sample rate: " << baseConfig.sampleRate << " Hz"
              << ", buffer: " << baseConfig.blockSize << " samples" << std::endl;
  } else {
    daw::LogLine() << "No audio device; using " << baseConfig.sampleRate
              << " Hz for offline timing" << std::endl;
    audioBackend.reset();
  }
  // --block-size wins over both, and it is applied AFTER the device probe so an offline render
  // is not silently given the device's buffer instead of the one it asked for. It exists so
  // block-size invariance is checkable through the real engine (§3.5).
  if (forcedBlockSize > 0) {
    baseConfig.blockSize = forcedBlockSize;
    daw::LogLine() << "Block size forced to " << baseConfig.blockSize << " samples" << std::endl;
  }
  // --sample-rate wins over the device too, and for the same reason: applied AFTER the probe so
  // an offline render is not silently handed whatever output happens to be selected. This is what
  // makes a render reproducible on a machine whose default device changes under it.
  if (forcedSampleRate > 0.0) {
    baseConfig.sampleRate = forcedSampleRate;
    daw::LogLine() << "Sample rate forced to " << baseConfig.sampleRate << " Hz" << std::endl;
  }

  return out;
}

int checkHostBinaryVersion() {
  // A STALE HOST BINARY IS DETECTED HERE, BEFORE ANY HOST IS SPAWNED.
  //
  // This started out just before the threads launch, which is TOO LATE: the tracks are set up
  // first and that is where hosts are connected, so the engine still died with 'waitForSocket
  // timed out' and the diagnostic never printed. A check that fires after the thing it explains
  // has already failed is not a check.
  //
  // juce_host_process is a SEPARATE CMake TARGET, so `cmake --build . --target daw_engine` after
  // a contract change leaves a host compiled against the old layout. What that looked like before
  // this check: the host fails to appear, the log fills with "connect(...) failed: No such file
  // or directory", and every symptom points somewhere else — the sockets, the plugin scan, the
  // read-back you just added. Two of us lost an hour to it on the same day, independently.
  //
  // The check is EXACT rather than a heuristic on file times: the host reports the versions it
  // was compiled against and exits. One fork at startup, and it turns an hour into a line.
  {
    const std::string hostExe = [] {
      if (const char* env = std::getenv("DAW_HOST_BINARY")) {
        if (env[0] != '\0') {
          return std::string(env);
        }
      }
      return std::string("./juce_host_process");
    }();
    std::string probe;
    if (FILE* pipe = ::popen((hostExe + " --version 2>/dev/null").c_str(), "r")) {
      char buf[128];
      while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        probe += buf;
      }
      ::pclose(pipe);
    }
    unsigned hostShm = 0, hostControl = 0;
    const bool parsed =
        std::sscanf(probe.c_str(), "shm=%u control=%u", &hostShm, &hostControl) == 2;
    if (!parsed) {
      // An OLDER host predates --version entirely, which is itself the answer. Not fatal — it
      // may be a deliberately pinned binary — but it is said out loud rather than discovered.
      daw::LogLine() << "Engine: WARNING could not read the host binary's contract version ("
                << hostExe << "). If it fails to start, rebuild ALL targets, not just "
                   "daw_engine." << std::endl;
      DAW_EVENT("host.version_unknown").field("binary", hostExe);
    } else if (hostShm != daw::kShmVersion || hostControl != daw::kControlVersion) {
      daw::LogLine() << "Engine: REFUSING TO START — the host binary is stale.\n"
                << "  " << hostExe << " was built against shm=" << hostShm
                << " control=" << hostControl << "\n"
                << "  this engine expects              shm=" << daw::kShmVersion
                << " control=" << daw::kControlVersion << "\n"
                << "  juce_host_process is a SEPARATE TARGET: build everything, not just "
                   "daw_engine.\n"
                << "      cmake --build build -j8" << std::endl;
      DAW_EVENT("host.version_mismatch")
          .field("binary", hostExe)
          .field("host_shm", hostShm)
          .field("host_control", hostControl)
          .field("engine_shm", static_cast<uint64_t>(daw::kShmVersion))
          .field("engine_control", static_cast<uint64_t>(daw::kControlVersion));
      return 1;
    }
  }
  return 0;
}

}  // namespace daw::engine
