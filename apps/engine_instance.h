#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

// WHERE ONE ENGINE INSTANCE'S PRIVATE PATHS COME FROM.
//
// Two engines on one machine used to share both of the paths below: /tmp/daw_host_track_<n>.sock
// and /daw_engine_shared_<n>, whose only name was the TRACK ID. So a second engine's host unlinked
// the first's socket and both mapped the same per-track segment — one engine's audio written into
// another's buffers, hosts answering to whichever engine connected last. It is not a test-harness
// problem: two projects open at once collide exactly the same way, and it is the same shape as the
// four-engines-on-one-segment failure that looked like the engine dying.
//
// An engine that was given a NAME (DAW_UI_SHM_NAME) gets private paths derived from it. An unnamed
// engine keeps the legacy names exactly, so the single-instance default and anything holding those
// literals are unchanged.
//
// THIS HEADER EXISTS BECAUSE A TEST HARDCODED "/daw_engine_shared" and broke the moment the engine
// started deriving the name. Duplicating the derivation in the test would have been the same bug
// waiting to happen again from the other side, so there is one definition and everyone calls it.
namespace daw {

// A short, stable token for this engine instance, or empty for an unnamed one. A hash rather than
// the name itself because shm_open on macOS takes 31 characters in total, and a readable name
// spends that budget on its own.
inline std::string engineInstanceToken() {
  const char* env = std::getenv("DAW_UI_SHM_NAME");
  if (!env || env[0] == '\0') {
    return {};
  }
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (const char* p = env; *p != '\0'; ++p) {
    h ^= static_cast<unsigned char>(*p);
    h *= 1099511628211ull;
  }
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08x", static_cast<uint32_t>(h ^ (h >> 32)));
  return std::string(buf);
}

// The engine<->host control socket for one track. DAW_HOST_SOCKET_PREFIX overrides it outright,
// which is what the ctest harnesses use to get a per-run path.
inline std::string trackSocketPath(uint32_t trackId) {
  if (const char* prefix = std::getenv("DAW_HOST_SOCKET_PREFIX")) {
    std::string base(prefix);
    if (!base.empty()) {
      return base + "_" + std::to_string(trackId) + ".sock";
    }
  }
  const std::string token = engineInstanceToken();
  if (!token.empty()) {
    return "/tmp/daw_host_" + token + "_" + std::to_string(trackId) + ".sock";
  }
  return "/tmp/daw_host_track_" + std::to_string(trackId) + ".sock";
}

// The engine<->host shared segment for one track. The engine passes this to the host as
// DAW_SHM_NAME, so the two cannot disagree about it.
inline std::string trackShmName(uint32_t trackId) {
  const std::string token = engineInstanceToken();
  if (!token.empty()) {
    // "/dawshm_" + 8 + "_" + up to 10 digits = 27 <= 31, the macOS shm_open limit.
    return "/dawshm_" + token + "_" + std::to_string(trackId);
  }
  if (trackId == 0) {
    return "/daw_engine_shared";
  }
  return "/daw_engine_shared_" + std::to_string(trackId);
}

// The UI observation segment. The name every other derivation above is keyed on.
inline std::string uiShmName() {
  if (const char* env = std::getenv("DAW_UI_SHM_NAME")) {
    std::string name(env);
    if (!name.empty() && name.front() != '/') {
      name.insert(name.begin(), '/');
    }
    return name;
  }
  return "/daw_engine_ui";
}

}  // namespace daw
