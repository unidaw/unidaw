// Tests for apps/engine_startup.h — the command line and the environment.
//
// THESE 150 LINES HAD NO DIRECT COVERAGE, and could not have had any: they were the first thing
// main() did, so reaching them meant starting an engine, and every check that starts an engine
// passes the flags it needs and asserts on what came out the far end. That kind of check can only
// see the flags it uses, in the position it puts them, and it reads a mis-parsed flag as a broken
// engine.
//
// TWO OF THE RULES BELOW ARE HERE BECAUSE THEY WERE ONCE WRONG:
//
//   A VALUELESS FLAG IN LAST POSITION. The loop bound used to be `i + 1 < argc`, so a flag that
//   takes no value was invisible when it came last and `daw_engine --no-spawn` silently spawned a
//   host. Every existing check passes --no-spawn in the middle of a longer line, which is exactly
//   the position that works. testTrailingValuelessFlag puts it last, on purpose.
//
//   AN OUT-OF-RANGE --sample-rate IS REFUSED. Not clamped, not ignored with a warning: a silent
//   fallback to the device rate is how the original defect stayed invisible for weeks, with the
//   render claiming to honour a rate it had discarded.
//
// The environment half is tested through setenv/unsetenv rather than by launching anything.
#include "apps/engine_startup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace daw::engine;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

namespace {

// argv as the real thing sees it: argv[0] is the program, and the array is mutable char*.
struct Argv {
  std::vector<std::string> store;
  std::vector<char*> ptrs;
  explicit Argv(std::vector<std::string> args) {
    store.reserve(args.size() + 1);
    store.push_back("daw_engine");
    for (auto& a : args) store.push_back(std::move(a));
    for (auto& s : store) ptrs.push_back(s.data());
  }
  int argc() const { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }
};

int parse(std::vector<std::string> args, EngineArgs& out) {
  Argv a(std::move(args));
  return parseEngineArgs(a.argc(), a.argv(), out);
}

// ------------------------------------------------------------------ defaults
void testDefaults() {
  EngineArgs out;
  CHECK(parse({}, out) == 0);
  CHECK(out.spawnHost);            // spawning is the default; --no-spawn turns it off
  CHECK(!out.noAudio);
  CHECK(out.runSeconds == -1);     // -1 means "no limit", distinct from 0
  CHECK(out.renderName.empty());   // empty is what makes the engine online
  CHECK(out.forcedBlockSize == 0u);
  CHECK(out.forcedSampleRate == 0.0);
  CHECK(out.startupProject.empty());
}

// ---------------------------------------------------- the trailing-flag rule
void testTrailingValuelessFlag() {
  // THE REGRESSION THIS FILE EXISTS FOR. Last position, nothing after it.
  EngineArgs out;
  CHECK(parse({"--no-spawn"}, out) == 0);
  CHECK(!out.spawnHost);

  EngineArgs out2;
  CHECK(parse({"--socket", "/tmp/s", "--no-audio"}, out2) == 0);
  CHECK(out2.noAudio);
  CHECK(out2.socketPath == "/tmp/s");
}

// A flag that NEEDS a value and does not get one must be ignored, not read past the end.
void testValueFlagWithoutValue() {
  EngineArgs out;
  out.socketPath = "seeded";
  CHECK(parse({"--socket"}, out) == 0);
  CHECK(out.socketPath == "seeded");  // untouched, and no read of argv[argc]
}

// ------------------------------------------------------------- value parsing
void testValueFlags() {
  EngineArgs out;
  CHECK(parse({"--socket", "/tmp/sock", "--render", "out.wav", "--project", "p.uniproj.json",
               "--run-seconds", "5", "--block-size", "64"}, out) == 0);
  CHECK(out.socketPath == "/tmp/sock");
  CHECK(out.renderName == "out.wav");
  CHECK(out.startupProject == "p.uniproj.json");
  CHECK(out.runSeconds == 5);
  CHECK(out.forcedBlockSize == 64u);
}

void testClamps() {
  EngineArgs out;
  CHECK(parse({"--run-seconds", "-5"}, out) == 0);
  CHECK(out.runSeconds == 0);  // clamped at 0, never negative

  EngineArgs out2;
  CHECK(parse({"--block-size", "0"}, out2) == 0);
  CHECK(out2.forcedBlockSize == 1u);  // a block of zero frames is not a block
}

// --plugin is made ABSOLUTE at parse time, because the engine changes nothing about its working
// directory afterwards but the host it spawns is a separate process.
void testPluginPathIsAbsolute() {
  EngineArgs out;
  CHECK(parse({"--plugin", "some/relative.vst3"}, out) == 0);
  CHECK(!out.pluginPath.empty());
  CHECK(std::filesystem::path(out.pluginPath).is_absolute());
  CHECK(out.pluginPath.find("some/relative.vst3") != std::string::npos);
}

// ------------------------------------------------------- the refusal, twice
void testSampleRateRefused() {
  EngineArgs low;
  CHECK(parse({"--sample-rate", "7999"}, low) == 2);
  CHECK(low.forcedSampleRate == 0.0);  // refused means NOT applied

  EngineArgs high;
  CHECK(parse({"--sample-rate", "384001"}, high) == 2);
  CHECK(high.forcedSampleRate == 0.0);

  EngineArgs ok;
  CHECK(parse({"--sample-rate", "48000"}, ok) == 0);
  CHECK(ok.forcedSampleRate == 48000.0);

  // The bounds are INCLUSIVE at both ends — a check that only tested the middle would pass with
  // the comparison written either way.
  EngineArgs edgeLow, edgeHigh;
  CHECK(parse({"--sample-rate", "8000"}, edgeLow) == 0);
  CHECK(edgeLow.forcedSampleRate == 8000.0);
  CHECK(parse({"--sample-rate", "384000"}, edgeHigh) == 0);
  CHECK(edgeHigh.forcedSampleRate == 384000.0);
}

// An unknown flag is not fatal, and it does not eat the flag after it.
void testUnknownFlagIgnored() {
  EngineArgs out;
  CHECK(parse({"--not-a-flag", "--no-audio"}, out) == 0);
  CHECK(out.noAudio);
}

// ------------------------------------------------------------- environment
struct EnvGuard {
  const char* name;
  std::string saved;
  bool had;
  explicit EnvGuard(const char* n) : name(n) {
    const char* v = std::getenv(n);
    had = v != nullptr;
    if (had) saved = v;
  }
  ~EnvGuard() {
    if (had) ::setenv(name, saved.c_str(), 1);
    else ::unsetenv(name);
  }
};

void testEnvironment() {
  EnvGuard g1("DAW_ENGINE_TEST_MODE"), g2("DAW_ENGINE_TEST_THROTTLE_MS");
  EnvGuard g3("DAW_PATCHER_PARALLEL"), g4("DAW_DISABLE_PDC"), g5("DAW_TRACE_NOTES");

  ::unsetenv("DAW_ENGINE_TEST_MODE");
  ::unsetenv("DAW_ENGINE_TEST_THROTTLE_MS");
  ::unsetenv("DAW_PATCHER_PARALLEL");
  ::unsetenv("DAW_DISABLE_PDC");
  ::unsetenv("DAW_TRACE_NOTES");
  EngineArgs off;
  readStartupEnvironment(off);
  CHECK(!off.testMode);
  CHECK(off.testThrottleMs == 0);
  CHECK(!off.patcherParallel);
  CHECK(!off.pdcDisabled);
  CHECK(!off.traceNotes);

  ::setenv("DAW_ENGINE_TEST_MODE", "1", 1);
  ::setenv("DAW_ENGINE_TEST_THROTTLE_MS", "7", 1);
  ::setenv("DAW_PATCHER_PARALLEL", "1", 1);
  ::setenv("DAW_DISABLE_PDC", "1", 1);
  ::setenv("DAW_TRACE_NOTES", "anything", 1);  // presence, not value
  EngineArgs on;
  readStartupEnvironment(on);
  CHECK(on.testMode);
  CHECK(on.testThrottleMs == 7);
  CHECK(on.patcherParallel);
  CHECK(on.pdcDisabled);
  CHECK(on.traceNotes);

  // "0" IS NOT "1". These three read the value, so setting them to anything else must leave them
  // off — an engine started with DAW_PATCHER_PARALLEL=0 that ran the parallel path would be a
  // silent change of the thing being measured.
  ::setenv("DAW_ENGINE_TEST_MODE", "0", 1);
  ::setenv("DAW_PATCHER_PARALLEL", "0", 1);
  ::setenv("DAW_DISABLE_PDC", "0", 1);
  EngineArgs zero;
  readStartupEnvironment(zero);
  CHECK(!zero.testMode);
  CHECK(!zero.patcherParallel);
  CHECK(!zero.pdcDisabled);

  // A throttle that is not a positive number leaves the default rather than becoming garbage.
  ::setenv("DAW_ENGINE_TEST_THROTTLE_MS", "not-a-number", 1);
  EngineArgs junk;
  readStartupEnvironment(junk);
  CHECK(junk.testThrottleMs == 0);
  ::setenv("DAW_ENGINE_TEST_THROTTLE_MS", "-3", 1);
  EngineArgs neg;
  readStartupEnvironment(neg);
  CHECK(neg.testThrottleMs == 0);
}

}  // namespace

int main() {
  testDefaults();
  testTrailingValuelessFlag();
  testValueFlagWithoutValue();
  testValueFlags();
  testClamps();
  testPluginPathIsAbsolute();
  testSampleRateRefused();
  testUnknownFlagIgnored();
  testEnvironment();

  if (g_fail != 0) {
    std::printf("engine_startup_tests: FAIL (%d)\n", g_fail);
    return 1;
  }
  std::printf("engine_startup_tests: PASS\n");
  return 0;
}
