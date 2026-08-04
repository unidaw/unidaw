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
