#pragma once

#include <cstdint>
#include <sstream>
#include <string>

namespace daw {

// One structured event per line (JSONL), so engine behaviour can be queried
// rather than grepped. Goes to stderr by default; set DAW_EVENT_LOG=<path> to
// send it to a file instead, which keeps it separate from plain diagnostics.
//
// NOT real-time safe: it allocates and does IO. Call it from the UI/control
// threads, never from the audio callback.
//
//   DAW_EVENT("clip.version_mismatch")
//       .field("base", baseVersion)
//       .field("current", current)
//       .field("track", trackId);
class LogEvent {
 public:
  explicit LogEvent(const char* name);
  ~LogEvent();

  LogEvent(const LogEvent&) = delete;
  LogEvent& operator=(const LogEvent&) = delete;

  LogEvent& field(const char* key, uint64_t value);
  LogEvent& field(const char* key, uint32_t value);
  LogEvent& field(const char* key, int64_t value);
  LogEvent& field(const char* key, int value);
  LogEvent& field(const char* key, double value);
  LogEvent& field(const char* key, bool value);
  LogEvent& field(const char* key, const std::string& value);
  LogEvent& field(const char* key, const char* value);

 private:
  void writeKey(const char* key);

  std::ostringstream buffer_;
};

// True when structured events are enabled (default on). Set DAW_EVENT_LOG_OFF=1
// to silence them without touching call sites.
bool eventLogEnabled();

#define DAW_EVENT(name) ::daw::LogEvent(name)

}  // namespace daw
